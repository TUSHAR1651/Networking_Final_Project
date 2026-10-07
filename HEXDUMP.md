# Annotated hexdump — one complete request and response

Captured with `./bserve ./www 9000` and `./bcurl -v localhost:9000/index.html`
(the `-v` output, unedited, is reproduced below each heading). Every byte is
accounted for. Offsets are relative to the start of each frame. All integers are
big-endian.

Totals: the client sent **75 bytes** (8 preface + 57 REQUEST + 10 GOAWAY); the
server sent **262 bytes** (136 RESPONSE + 126 DATA). For comparison, the same
request in HTTP/1.1 text is 86 bytes (`GET /index.html HTTP/1.1\r\nHost: …`);
this REQUEST frame is 57.

---

## 1. Client → server: connection preface (8 bytes)

```
00000000  42 48 54 54 50 2f 31 0a                           |BHTTP/1.|
```

| Offset | Bytes | Meaning |
|---|---|---|
| 0–7 | `42 48 54 54 50 2f 31 0a` | ASCII `BHTTP/1\n`. Sent once, before the first frame. Not a frame: it has no header. |

## 2. Client → server: REQUEST frame (8 + 49 = 57 bytes)

```
00000000  00 00 31 01 01 00 00 01  01 00 0b 2f 69 6e 64 65  |..1......../inde|
00000010  78 2e 68 74 6d 6c 01 00  0e 6c 6f 63 61 6c 68 6f  |x.html...localho|
00000020  73 74 3a 39 30 30 30 02  00 09 62 63 75 72 6c 2f  |st:9000...bcurl/|
00000030  31 2e 30 03 00 03 2a 2f  2a                       |1.0...*/*|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0–2 | `00 00 31` | Length (24) | payload is 0x31 = **49** bytes |
| 3 | `01` | Type (8) | **REQUEST** |
| 4 | `01` | Flags (8) | **END**: no body follows (always, in v1) |
| 5–7 | `00 00 01` | Request ID (24) | request **1** on this connection |
| 8 | `01` | Method | **GET** |
| 9–10 | `00 0b` | Path length | 11 |
| 11–21 | `2f 69 … 6c` | Path | `/index.html` |
| 22 | `01` | Header index | static **1 = host** |
| 23–24 | `00 0e` | Value length | 14 |
| 25–38 | `6c 6f … 30` | Value | `localhost:9000` |
| 39 | `02` | Header index | static **2 = user-agent** |
| 40–41 | `00 09` | Value length | 9 |
| 42–50 | `62 63 … 30` | Value | `bcurl/1.0` |
| 51 | `03` | Header index | static **3 = accept** |
| 52–53 | `00 03` | Value length | 3 |
| 54–56 | `2a 2f 2a` | Value | `*/*` |

Check: 3 (method + path length) + 11 + (3 + 14) + (3 + 9) + (3 + 3) = 49 = Length.
The header block ends at the end of the payload; there is no count and no terminator.

## 3. Server → client: RESPONSE frame (8 + 128 = 136 bytes)

```
00000000  00 00 80 02 00 00 00 01  00 c8 04 00 18 74 65 78  |.............tex|
00000010  74 2f 68 74 6d 6c 3b 20  63 68 61 72 73 65 74 3d  |t/html; charset=|
00000020  75 74 66 2d 38 05 00 03  31 31 38 06 00 1d 57 65  |utf-8...118...We|
00000030  64 2c 20 30 37 20 4f 63  74 20 32 30 32 36 20 31  |d, 07 Oct 2026 1|
00000040  36 3a 35 37 3a 33 38 20  47 4d 54 07 00 0d 22 37  |6:57:38 GMT..."7|
00000050  36 2d 36 61 63 36 37 61  30 32 22 08 00 1d 57 65  |6-6ac67a02"...We|
00000060  64 2c 20 30 37 20 4f 63  74 20 32 30 32 36 20 31  |d, 07 Oct 2026 1|
00000070  37 3a 30 35 3a 35 37 20  47 4d 54 09 00 0a 62 73  |7:05:57 GMT...bs|
00000080  65 72 76 65 2f 31 2e 30                           |erve/1.0|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0–2 | `00 00 80` | Length | payload is 0x80 = **128** bytes |
| 3 | `02` | Type | **RESPONSE** |
| 4 | `00` | Flags | END clear: DATA frames follow |
| 5–7 | `00 00 01` | Request ID | answers request **1** |
| 8–9 | `00 c8` | Status (16) | 0xC8 = **200** |
| 10 | `04` | Header index | **4 = content-type** |
| 11–12 | `00 18` | Value length | 24 |
| 13–36 | `74 65 … 38` | Value | `text/html; charset=utf-8` |
| 37 | `05` | Header index | **5 = content-length** |
| 38–39 | `00 03` | Value length | 3 |
| 40–42 | `31 31 38` | Value | `118`, matching the DATA below |
| 43 | `06` | Header index | **6 = last-modified** |
| 44–45 | `00 1d` | Value length | 29 |
| 46–74 | `57 65 … 54` | Value | `Wed, 07 Oct 2026 16:57:38 GMT` |
| 75 | `07` | Header index | **7 = etag** |
| 76–77 | `00 0d` | Value length | 13 |
| 78–90 | `22 37 … 22` | Value | `"76-6ac67a02"` (size 0x76 = 118, mtime in hex) |
| 91 | `08` | Header index | **8 = date** |
| 92–93 | `00 1d` | Value length | 29 |
| 94–122 | `57 65 … 54` | Value | `Wed, 07 Oct 2026 17:05:57 GMT` |
| 123 | `09` | Header index | **9 = server** |
| 124–125 | `00 0a` | Value length | 10 |
| 126–135 | `62 73 … 30` | Value | `bserve/1.0` |

Check: 2 + (3 + 24) + (3 + 3) + (3 + 29) + (3 + 13) + (3 + 29) + (3 + 10) = 128 = Length.
Every name here hit the static table, so each one cost **1 byte**. As text, the
names alone would be 53 bytes, plus `": "` and CRLF per line.

## 4. Server → client: DATA frame (8 + 118 = 126 bytes)

```
00000000  00 00 76 00 01 00 00 01  3c 21 64 6f 63 74 79 70  |..v.....<!doctyp|
00000010  65 20 68 74 6d 6c 3e 0a  3c 74 69 74 6c 65 3e 62  |e html>.<title>b|
00000020  73 65 72 76 65 3c 2f 74  69 74 6c 65 3e 0a 3c 68  |serve</title>.<h|
00000030  31 3e 48 65 6c 6c 6f 20  66 72 6f 6d 20 62 73 65  |1>Hello from bse|
00000040  72 76 65 3c 2f 68 31 3e  0a 3c 70 3e 54 68 69 73  |rve</h1>.<p>This|
00000050  20 70 61 67 65 20 63 72  6f 73 73 65 64 20 74 68  | page crossed th|
00000060  65 20 77 69 72 65 20 61  73 20 42 48 54 54 50 2f  |e wire as BHTTP/|
00000070  31 20 66 72 61 6d 65 73  2e 3c 2f 70 3e 0a        |1 frames.</p>.|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0–2 | `00 00 76` | Length | **118** bytes of body |
| 3 | `00` | Type | **DATA** |
| 4 | `01` | Flags | **END**: last frame of response 1 (the body fits in one frame) |
| 5–7 | `00 00 01` | Request ID | body of request **1** |
| 8–125 | `3c 21 … 3e 0a` | Payload | the file `www/index.html`, verbatim, no escaping |

The response is complete: RESPONSE plus all DATA frames up to the one with END.
118 body bytes = content-length. The connection stays open for request 2.

## 5. Client → server: GOAWAY frame (8 + 2 = 10 bytes)

```
00000000  00 00 02 03 00 00 00 00  00 00                    |..........|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0–2 | `00 00 02` | Length | 2 |
| 3 | `03` | Type | **GOAWAY** |
| 4 | `00` | Flags | none |
| 5–7 | `00 00 00` | Request ID | **0**: connection-level frame, not tied to any request |
| 8–9 | `00 00` | Error code | **NO_ERROR**; no reason text follows |

The client had no more URLs, so it said goodbye and closed. The server closed its side in response.
