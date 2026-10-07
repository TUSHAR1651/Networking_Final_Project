# BHTTP/1 — HTTP in binary frames

**Status:** version 1 · **Audience:** anyone writing a client or a server from this page alone.
The keywords MUST, SHOULD and MAY are used as in RFC 2119. HTTP *semantics* (methods, status
codes, header meanings) are unchanged. This spec only replaces HTTP/1.1's text *syntax*.

## 1. Model

A client opens **one TCP connection** and sends **many requests** over it, one after another.
Each message is a sequence of **frames**: a REQUEST or RESPONSE frame, then zero or more DATA
frames. The last frame of a message carries the **END** flag. The server answers requests
**in the order received**, so a client MAY send the next request before the previous response
finishes (pipelining). All integers are **unsigned, big-endian** (network order).

**Preface.** Before its first frame, the client sends the 8 bytes `42 48 54 54 50 2F 31 0A`
(`"BHTTP/1\n"`). If the server reads anything else, or nothing within its idle timeout, it SHOULD
send GOAWAY(PROTOCOL_ERROR) and MUST close. The server sends no preface. A future version changes the digit, so a v1 server
refuses a v2 client up front rather than misreading its frames.

## 2. Frame header — 8 bytes, fixed

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-----------------------------------------------+---------------+
|                  Length (24)                  |   Type (8)    |
+---------------+-----------------------------------------------+
|   Flags (8)   |                Request ID (24)                |
+---------------+-----------------------------------------------+
|                    Payload (Length bytes) ...                 |
```

| Field | Bits | Meaning |
|---|---|---|
| Length | 24 | Payload size in bytes, **excluding** these 8. 0 – 16 777 215. |
| Type | 8 | Frame type (§3). |
| Flags | 8 | Per-type bits. `0x01` = **END**. All other bits are reserved: senders MUST send 0, receivers MUST ignore them. |
| Request ID | 24 | Which request this frame belongs to. 0 = the connection itself: only GOAWAY uses it, and a REQUEST on ID 0 is a protocol error (§5). The client numbers requests 1, 2, 3, …; the server echoes the ID. After 2²⁴−1 the ID wraps to 1. That is safe because responses arrive in order, so IDs only need to be unique among requests still in flight. |

**Why these widths.** The header is **exactly 64 bits**, so it is one aligned load and one
`memcpy` into a struct. HTTP/2's header (24/8/8/1+31) is 9 bytes. HTTP/2 needs 31-bit stream IDs because
it multiplexes many concurrent streams and never reuses an ID on a long-lived connection.
v1 only has to match responses to in-order requests, so **24 bits** of ID is plenty, and
dropping 8 bits buys the alignment. **Length = 24 bits** is the same choice HTTP/2 made, for the
same reason. 16 bits would cap a frame at 64 KiB. That is too tight for a large header block and leaves
no headroom for future frame types. 32 bits would let one header demand a 4 GiB buffer.
Large bodies are split into DATA frames anyway, so the extra byte buys nothing. **Type = 8
bits**: v1 uses 4 of 256 values, so there is ample room for v2. **Flags = 8 bits**: v1 uses one.

## 3. Frame types

| Type | Name | Direction | Payload |
|---|---|---|---|
| `0x00` | DATA | both | Body bytes, verbatim. No escaping, no chunk syntax. |
| `0x01` | REQUEST | client → server | `Method(8) PathLen(16) Path(PathLen) HeaderBlock` |
| `0x02` | RESPONSE | server → client | `Status(16) HeaderBlock` |
| `0x03` | GOAWAY | both, ID 0 | `ErrorCode(16) Reason(UTF-8, rest of payload, may be empty)` |
| other | — | — | **Reserved. See the rule below.** |

> **The extension rule (MUST).** A receiver that meets a frame type it does not know MUST read
> and discard exactly `Length` payload bytes, then carry on as if the frame had never been sent.
> It MUST NOT reply, error, or close. The same applies to unknown flag bits (ignore them) and to
> unknown header indices (§4: skip the entry). A known type sent in the wrong direction
> (e.g. a client-sent RESPONSE) is likewise skipped. A receiver never needs to understand a frame to
> find where the next one starts. This is what leaves room for version 2.

**REQUEST.** `Method`: `0x01` GET, `0x02` HEAD; a server answers any other value with 501.
`Path` is raw bytes, e.g. `/docs/a b.html`. It MUST start with `/` and MUST NOT contain NUL. It
is length-prefixed, so no percent-encoding is needed, and servers MUST NOT decode it (`%20` is
three literal bytes). A server ignores everything from the first `?`; `#` has no special meaning. v1 requests have no body: the client MUST set END. A server answers as soon as the REQUEST
arrives, whatever its flags, and MUST ignore DATA frames it receives. A server MUST accept REQUEST payloads up to **16 384 bytes**; a larger one is
skipped and answered with 400.

**RESPONSE.** `Status` is the HTTP status code (200, 404, …). There is no reason phrase. END is set when no body
follows: always for HEAD, and for an empty body. Otherwise the body follows as DATA frames
with the same ID; senders SHOULD keep each DATA payload ≤ 16 384 bytes. A zero-length DATA
frame with END is legal and ends the body. `content-length` SHOULD be sent. It gives the size of the body a GET
would receive: for GET the DATA bytes MUST add up to it; for HEAD no body is sent and the
value is not checked.

**GOAWAY.** `ErrorCode`: `0` NO_ERROR, `1` PROTOCOL_ERROR, `2` IDLE_TIMEOUT. The sender closes
the connection after sending it; the receiver MUST stop sending and close. A client SHOULD send
GOAWAY(NO_ERROR) before closing. A request with no complete response before a GOAWAY was
not served (any code), and the client MAY retry it on a new connection. A server MAY close a
connection that has been idle for 30 s or more, sending GOAWAY(IDLE_TIMEOUT) first.

## 4. Header block

Header entries run back-to-back until the payload ends. There is no count and no terminator:
the frame Length already bounds the block.

```
Index(8) [ NameLen(8) Name(NameLen) ]  ValueLen(16) Value(ValueLen)
          └─ present only if Index = 0 ┘
```

* **Index 1–10**: a name from the static table below. Costs 1 byte instead of the full name.
* **Index 0**: a literal name follows: 1–255 bytes, lowercase ASCII.
* **Index 11–255**: reserved for v2. The receiver MUST still read `ValueLen` and `Value`
  (the layout is identical) and then **ignore the entry**.
* `Value` is 0–65 535 bytes. HTTP semantics apply: `content-length` is ASCII decimal, and dates
  use the IMF-fixdate format. A name MAY repeat.

| # | Name | Sent by | | # | Name | Sent by |
|---|---|---|---|---|---|---|
| 1 | host | client | | 6 | last-modified | server |
| 2 | user-agent | client | | 7 | etag | server |
| 3 | accept | client | | 8 | date | server |
| 4 | content-type | server | | 9 | server | server |
| 5 | content-length | server | | 10 | allow | server (501) |

These are the ten names this implementation actually sends. Together they make up HPACK's
first two mechanisms: a static table and length-prefixed literals. This spec deliberately leaves
out HPACK's dynamic table and Huffman coding. Those add state shared across the connection, and with it
a whole class of bugs. In a typical request every name is a table hit, so a request costs one
byte per name.

## 5. Errors

| Situation | Server does | Connection |
|---|---|---|
| REQUEST payload cannot be parsed: shorter than 3 bytes, `PathLen` or a header entry overruns the payload, literal `NameLen` = 0, path empty / not starting with `/` / containing NUL / too long for the server's filesystem, or payload > 16 384 bytes | **400**, same Request ID | **stays open**. Length was valid, so the next frame boundary is known. |
| File does not exist, is not a regular file, or resolving outside the root (`..`, symlinks) | **404**. A directory serves its `index.html`. | stays open |
| File unreadable (permissions) | 403 | stays open |
| Method not GET/HEAD | **501** with `allow: GET, HEAD` | stays open |
| Bad or missing preface; REQUEST on Request ID 0 | GOAWAY(PROTOCOL_ERROR) | closed |
| EOF in the middle of a frame | — | closed |

Checks apply in table order, so a malformed frame gets 400 even if its method is also unknown.
Error responses (all codes) carry a one-line `text/plain` body (except for HEAD). A **client** MUST treat
these as protocol errors and close the connection: a RESPONSE or DATA frame whose ID is not the
request it is waiting on, DATA before RESPONSE, a RESPONSE payload shorter than 2 bytes, a
malformed header block, or a body whose length disagrees with `content-length`.

**`bcurl` exit status:** `2` for usage errors; otherwise `1` if a network or protocol error
happened (this wins over any status); else `5` if any status was ≥ 500, `4` if any was 400–499,
`0` otherwise. All URLs on
one command line go over **one** connection, and bcurl never opens a second.

## 6. Example

`GET /index.html`, host `localhost:9000`. 57 bytes (plus the 8-byte preface, once per connection), versus 86 bytes in HTTP/1.1:

```
00 00 31 01 01 00 00 01     Length=49  Type=REQUEST  Flags=END  ID=1
01 00 0b "/index.html"      GET, path length 11, path
01 00 0e "localhost:9000"   host        (static #1)
02 00 09 "bcurl/1.0"        user-agent  (static #2)
03 00 03 "*/*"              accept      (static #3)
```

Every byte of a full exchange is annotated in **HEXDUMP.md**.
