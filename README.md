# BHTTP/1 — HTTP, in binary

A course project in two tracks, joined by one protocol:

| What | File |
|---|---|
| **1. The spec** (two pages, enough for a stranger) | [SPEC.md](SPEC.md) |
| **2. The programs**: `bserve` (track 1, server) and `bcurl` (track 2, client) | [src/](src/) |
| **3. An annotated hexdump** of one complete request and response | [HEXDUMP.md](HEXDUMP.md) |
| Conformance tests (an independent Python implementation of the spec) | [tests/test_bhttp.py](tests/test_bhttp.py) |

## Build

Needs a C11 compiler and `make` (tested with Apple clang on macOS and gcc on Linux). No dependencies.

```sh
make            # builds ./bserve and ./bcurl
make test       # 33 conformance tests (needs python3, standard library only)
```

## Run

```sh
./bserve ./www 9000                         # terminal 1: serve ./www on port 9000

./bcurl localhost:9000/index.html           # terminal 2: body to stdout
./bcurl -v localhost:9000/index.html        # also hexdump every frame to stderr
./bcurl -I localhost:9000/hello.txt         # HEAD
./bcurl -H 'X-Note: hi' localhost:9000/     # extra header (sent as a literal name)
./bcurl localhost:9000/a localhost:9000/hello.txt   # two requests, ONE connection
echo $?                                     # 4: /a was a 404
```

`bcurl` exit codes: `0` all OK · `4` some 4xx · `5` some 5xx · `1` network/protocol error · `2` usage.

## What the programs do

**bserve**: accepts TCP connections (one process each), checks the preface, then reads
frames in a loop. Each REQUEST maps its path to a file under the root and gets a RESPONSE plus
DATA frames of up to 16 KiB each. Errors: **404** for files that don't exist or resolve outside
the root, **400** for a malformed frame, **501** for an unknown method. Unknown frame types are
skipped. The connection stays open after every response, errors included, until the client sends
GOAWAY or closes, or it has been idle for 30 s.

**bcurl**: builds REQUEST frames (host / user-agent / accept from the static table, `-H`
extras as literals), writes the body to stdout, skips unknown frames, hexdumps every frame both
ways with `-v`, and exits non-zero on 4xx/5xx. Every URL on the command line uses the same
single connection.

## Verified

* `make test`: 33 tests. Against `bserve`: keep-alive, 404/400/501 with the connection
  surviving, nine kinds of malformed frame, unknown frame types (including a 70 KB one),
  unknown flags and header indices, path traversal and symlink escape, HEAD, empty and 100 KB
  files, pipelining, byte-at-a-time delivery, concurrent clients. Against `bcurl`, using a
  separate Python server: unknown frames injected everywhere, odd DATA splits, literal headers,
  exit codes, one connection for many URLs, and protocol-error handling.
* Passes 20 runs in a row, under AddressSanitizer + UBSan, and on Linux/gcc (Docker `gcc:latest`).

## Layout

```
SPEC.md  HEXDUMP.md  README.md  Makefile
src/proto.h, src/proto.c   frame header, header block, I/O, hexdump (shared)
src/bserve.c               the server
src/bcurl.c                the client
tests/test_bhttp.py        conformance tests
www/                       sample site
```
