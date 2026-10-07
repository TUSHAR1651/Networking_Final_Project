"""Conformance tests for BHTTP/1 (see SPEC.md).

This file contains a second, independent implementation of the wire format,
written from SPEC.md alone, in Python:

  * as a CLIENT it drives ./bserve with hand-built (and deliberately broken) frames;
  * as a SERVER it answers ./bcurl, injecting unknown frame types, literal and
    unknown-index headers, odd DATA splits and error statuses.

Run with:  make test     (or: python3 tests/test_bhttp.py -v)
"""

import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BSERVE = os.path.join(ROOT, "bserve")
BCURL = os.path.join(ROOT, "bcurl")

PREFACE = b"BHTTP/1\n"
DATA, REQUEST, RESPONSE, GOAWAY = 0x00, 0x01, 0x02, 0x03
END = 0x01
GET, HEAD = 0x01, 0x02
STATIC = [None, "host", "user-agent", "accept", "content-type", "content-length",
          "last-modified", "etag", "date", "server", "allow"]


# ---- wire format, from the spec ---------------------------------------------

def frame(ftype, flags, rid, payload=b""):
    assert len(payload) < 1 << 24 and rid < 1 << 24
    return (struct.pack(">I", len(payload))[1:] + bytes([ftype, flags])
            + struct.pack(">I", rid)[1:] + payload)


def hdr(name, value):
    """Encode one header entry; static index when the name is in the table."""
    v = value.encode() if isinstance(value, str) else value
    if isinstance(name, int):                     # raw index (used for "unknown index" tests)
        return bytes([name]) + struct.pack(">H", len(v)) + v
    if name in STATIC:
        return bytes([STATIC.index(name)]) + struct.pack(">H", len(v)) + v
    n = name.encode()
    return b"\x00" + bytes([len(n)]) + n + struct.pack(">H", len(v)) + v


def request_payload(method, path, headers=()):
    p = path.encode() if isinstance(path, str) else path
    return bytes([method]) + struct.pack(">H", len(p)) + p + b"".join(headers)


def parse_headers(block):
    out, i = [], 0
    while i < len(block):
        idx = block[i]
        i += 1
        if idx == 0:
            nl = block[i]
            name = block[i + 1:i + 1 + nl].decode()
            i += 1 + nl
        else:
            name = STATIC[idx] if idx < len(STATIC) else None
        (vl,) = struct.unpack(">H", block[i:i + 2])
        value = block[i + 2:i + 2 + vl].decode()
        i += 2 + vl
        if name is not None:
            out.append((name, value))
    return out


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("peer closed after %d of %d bytes" % (len(buf), n))
        buf += chunk
    return buf


def recv_frame(sock):
    h = recv_exact(sock, 8)
    length = int.from_bytes(h[0:3], "big")
    return h[3], h[4], int.from_bytes(h[5:8], "big"), recv_exact(sock, length)


class Response:
    def __init__(self, status, headers, body, data_frames, response_flags):
        self.status, self.headers, self.body = status, dict(headers), body
        self.data_frames, self.response_flags = data_frames, response_flags


def read_response(sock, rid):
    ftype, flags, fid, payload = recv_frame(sock)
    assert ftype == RESPONSE, "expected RESPONSE, got type %d" % ftype
    assert fid == rid, "response id %d != request id %d" % (fid, rid)
    status = struct.unpack(">H", payload[:2])[0]
    headers = parse_headers(payload[2:])
    body, data_frames, end = b"", [], flags & END
    while not end:
        ftype, dflags, fid, payload = recv_frame(sock)
        assert ftype == DATA and fid == rid
        data_frames.append((dflags, len(payload)))
        body += payload
        end = dflags & END
    return Response(status, headers, body, data_frames, flags)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# ---- bserve tests -------------------------------------------------------------

class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = cls.tmp.name
        cls.www = os.path.join(base, "www")
        os.makedirs(os.path.join(cls.www, "sub"))
        with open(os.path.join(cls.www, "index.html"), "w") as f:
            f.write("<h1>root index</h1>\n")
        with open(os.path.join(cls.www, "sub", "index.html"), "w") as f:
            f.write("<h1>sub index</h1>\n")
        with open(os.path.join(cls.www, "empty.txt"), "w"):
            pass
        cls.big = os.urandom(100_000)
        with open(os.path.join(cls.www, "big.bin"), "wb") as f:
            f.write(cls.big)
        with open(os.path.join(base, "secret.txt"), "w") as f:
            f.write("outside the root\n")
        os.symlink(os.path.join(base, "secret.txt"), os.path.join(cls.www, "escape.txt"))

        cls.port = free_port()
        cls.proc = subprocess.Popen([BSERVE, cls.www, str(cls.port)],
                                    stderr=subprocess.PIPE, stdout=subprocess.DEVNULL)
        # first launch of a freshly linked binary can be slow (macOS scans it)
        deadline = time.time() + 10
        while True:
            try:
                socket.create_connection(("127.0.0.1", cls.port), timeout=0.2).close()
                break
            except OSError:
                if time.time() > deadline or cls.proc.poll() is not None:
                    cls.proc.kill()
                    raise RuntimeError("bserve did not start listening")
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        cls.proc.stderr.close()
        cls.tmp.cleanup()

    def connect(self, preface=PREFACE):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.addCleanup(s.close)
        s.sendall(preface)
        return s

    def get(self, sock, path, rid=1, method=GET, headers=(), flags=END):
        sock.sendall(frame(REQUEST, flags, rid, request_payload(method, path, headers)))
        return read_response(sock, rid)

    def assert_still_open(self, sock, rid=99):
        r = self.get(sock, "/index.html", rid=rid)
        self.assertEqual(r.status, 200)

    def test_get_file(self):
        r = self.get(self.connect(), "/index.html")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, b"<h1>root index</h1>\n")
        self.assertEqual(r.headers["content-length"], str(len(r.body)))
        self.assertEqual(r.headers["content-type"], "text/html; charset=utf-8")
        for name in ("date", "server", "last-modified", "etag"):
            self.assertIn(name, r.headers)

    def test_keep_alive_many_requests(self):
        s = self.connect()
        for rid in range(1, 6):
            self.assertEqual(self.get(s, "/index.html", rid=rid).status, 200)

    def test_404_keeps_connection(self):
        s = self.connect()
        r = self.get(s, "/does-not-exist", rid=1)
        self.assertEqual(r.status, 404)
        self.assertEqual(r.body, b"404 not found\n")
        self.assert_still_open(s, rid=2)

    def test_directory_serves_index(self):
        s = self.connect()
        self.assertEqual(self.get(s, "/", rid=1).body, b"<h1>root index</h1>\n")
        self.assertEqual(self.get(s, "/sub", rid=2).body, b"<h1>sub index</h1>\n")
        self.assertEqual(self.get(s, "/sub/", rid=3).body, b"<h1>sub index</h1>\n")

    def test_query_string_ignored(self):
        self.assertEqual(self.get(self.connect(), "/index.html?x=1").status, 200)

    def test_large_file_split_into_data_frames(self):
        r = self.get(self.connect(), "/big.bin")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, self.big)
        self.assertGreater(len(r.data_frames), 1)
        self.assertTrue(all(n <= 16384 for _, n in r.data_frames))
        self.assertTrue(all(not f & END for f, _ in r.data_frames[:-1]))
        self.assertTrue(r.data_frames[-1][0] & END)

    def test_empty_file_end_on_response(self):
        r = self.get(self.connect(), "/empty.txt")
        self.assertEqual(r.status, 200)
        self.assertTrue(r.response_flags & END)
        self.assertEqual(r.data_frames, [])
        self.assertEqual(r.headers["content-length"], "0")

    def test_head(self):
        s = self.connect()
        r = self.get(s, "/big.bin", method=HEAD)
        self.assertEqual(r.status, 200)
        self.assertTrue(r.response_flags & END)
        self.assertEqual(r.body, b"")
        self.assertEqual(r.headers["content-length"], str(len(self.big)))
        self.assertEqual(self.get(s, "/nope", rid=2, method=HEAD).body, b"")
        self.assert_still_open(s, rid=3)

    def test_unknown_method_501(self):
        s = self.connect()
        r = self.get(s, "/index.html", method=0x07)
        self.assertEqual(r.status, 501)
        self.assertEqual(r.headers["allow"], "GET, HEAD")
        self.assert_still_open(s, rid=2)

    def test_path_traversal_and_symlink_escape(self):
        s = self.connect()
        self.assertEqual(self.get(s, "/../secret.txt", rid=1).status, 404)
        self.assertEqual(self.get(s, "/sub/../../secret.txt", rid=2).status, 404)
        self.assertEqual(self.get(s, "/escape.txt", rid=3).status, 404)
        self.assertEqual(self.get(s, "/sub/../index.html", rid=4).status, 200)

    def test_unknown_frame_types_are_skipped(self):
        s = self.connect()
        junk = (frame(0x7F, 0xFF, 0, b"future")
                + frame(0xE3, 0x00, 1, os.urandom(70_000))   # bigger than any known frame
                + frame(0x10, 0x00, 5, b""))
        s.sendall(junk + frame(REQUEST, END, 1, request_payload(GET, "/index.html")))
        self.assertEqual(read_response(s, 1).status, 200)

    def test_client_data_frames_ignored(self):
        s = self.connect()
        s.sendall(frame(DATA, END, 1, b"stray body"))
        self.assert_still_open(s, rid=1)

    def test_unknown_flags_ignored(self):
        self.assertEqual(self.get(self.connect(), "/index.html", flags=0xFF).status, 200)

    def test_literal_and_unknown_index_headers(self):
        headers = [hdr("host", "localhost"), hdr("x-custom-thing", "yes"),
                   hdr(0x55, "unknown index, value still skippable"), hdr("accept", "*/*")]
        self.assertEqual(self.get(self.connect(), "/index.html", headers=headers).status, 200)

    def test_malformed_frames_get_400_and_connection_survives(self):
        cases = {
            "too short": b"\x01\x00",
            "path length overruns": b"\x01\x00\xff/index.html",
            "value length overruns": request_payload(GET, "/index.html", [b"\x01\x00\x50ab"]),
            "literal name length 0": request_payload(GET, "/index.html", [b"\x00\x00\x00\x00"]),
            "truncated literal name": request_payload(GET, "/index.html", [b"\x00\x09abc"]),
            "missing value length": request_payload(GET, "/index.html", [b"\x03\x00"]),
            "no leading slash": request_payload(GET, "index.html"),
            "empty path": request_payload(GET, ""),
            "NUL in path": request_payload(GET, b"/index.html\x00.txt"),
        }
        s = self.connect()
        rid = 1
        for name, payload in cases.items():
            with self.subTest(case=name):
                s.sendall(frame(REQUEST, END, rid, payload))
                r = read_response(s, rid)
                self.assertEqual(r.status, 400)
                rid += 1
        self.assert_still_open(s, rid=rid)

    def test_request_id_zero_is_protocol_error(self):
        s = self.connect()
        s.sendall(frame(REQUEST, END, 0, request_payload(GET, "/index.html")))
        ftype, _, rid, payload = recv_frame(s)
        self.assertEqual((ftype, rid), (GOAWAY, 0))
        self.assertEqual(struct.unpack(">H", payload[:2])[0], 1)   # PROTOCOL_ERROR
        self.assertEqual(s.recv(1), b"")

    def test_oversized_request_frame_400(self):
        s = self.connect()
        payload = request_payload(GET, "/index.html", [hdr("x-pad", "a" * 20_000)])
        self.assertGreater(len(payload), 16384)
        s.sendall(frame(REQUEST, END, 1, payload))
        self.assertEqual(read_response(s, 1).status, 400)
        self.assert_still_open(s, rid=2)

    def test_bad_preface_goaway_and_close(self):
        s = self.connect(preface=b"GET / HTTP/1.1\r\n\r\n")
        ftype, _, rid, payload = recv_frame(s)
        self.assertEqual((ftype, rid), (GOAWAY, 0))
        self.assertEqual(struct.unpack(">H", payload[:2])[0], 1)   # PROTOCOL_ERROR
        self.assertEqual(s.recv(1), b"")

    def test_client_goaway_closes(self):
        s = self.connect()
        s.sendall(frame(GOAWAY, 0, 0, b"\x00\x00"))
        self.assertEqual(s.recv(1), b"")

    def test_frame_split_across_tcp_writes(self):
        s = self.connect()
        raw = frame(REQUEST, END, 1, request_payload(GET, "/index.html", [hdr("host", "x")]))
        for b in raw:
            s.sendall(bytes([b]))
            time.sleep(0.001)
        self.assertEqual(read_response(s, 1).status, 200)

    def test_pipelined_requests_answered_in_order(self):
        s = self.connect()
        s.sendall(b"".join(frame(REQUEST, END, rid, request_payload(GET, "/index.html"))
                           for rid in (1, 2, 3)))
        for rid in (1, 2, 3):
            self.assertEqual(read_response(s, rid).status, 200)

    def test_concurrent_connections(self):
        a, b = self.connect(), self.connect()
        a.sendall(frame(REQUEST, END, 1, request_payload(GET, "/big.bin")))
        self.assertEqual(self.get(b, "/index.html").status, 200)   # b not blocked by a
        self.assertEqual(read_response(a, 1).body, self.big)

    def test_bcurl_against_bserve(self):
        out = subprocess.run([BCURL, "localhost:%d/big.bin" % self.port], capture_output=True,
                             timeout=10)
        self.assertEqual(out.returncode, 0)
        self.assertEqual(out.stdout, self.big)
        out = subprocess.run([BCURL, "127.0.0.1:%d/nope" % self.port], capture_output=True,
                             timeout=10)
        self.assertEqual(out.returncode, 4)


# ---- bcurl tests, against an independent scripted server -------------------------

class FakeServer:
    """Minimal BHTTP/1 server. `respond(path, rid)` returns the raw bytes to send."""

    def __init__(self, respond):
        self.respond = respond
        self.connections = 0
        self.requests = []          # (rid, method, path, headers)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen()
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        with conn:
            try:
                assert recv_exact(conn, 8) == PREFACE
                while True:
                    ftype, _, rid, payload = recv_frame(conn)
                    if ftype == GOAWAY:
                        return
                    if ftype != REQUEST:
                        continue
                    (plen,) = struct.unpack(">H", payload[1:3])
                    path = payload[3:3 + plen].decode()
                    self.requests.append((rid, payload[0], path, parse_headers(payload[3 + plen:])))
                    conn.sendall(self.respond(path, rid))
            except (EOFError, OSError):
                return

    def close(self):
        self.sock.close()


def response(rid, status, body=b"", headers=None, chunks=None, extra_before=b"", extra_mid=b""):
    hs = headers if headers is not None else [hdr("content-length", str(len(body)))]
    head = frame(RESPONSE, 0 if body or chunks else END, rid,
                 struct.pack(">H", status) + b"".join(hs))
    parts = chunks if chunks is not None else ([body] if body else [])
    data = b"".join(frame(DATA, END if i == len(parts) - 1 else 0, rid, c)
                    for i, c in enumerate(parts))
    return extra_before + head + extra_mid + data


class ClientTests(unittest.TestCase):
    def serve(self, respond):
        srv = FakeServer(respond)
        self.addCleanup(srv.close)
        return srv

    def bcurl(self, *args):
        return subprocess.run([BCURL, *args], capture_output=True, timeout=10)

    def test_basic_fetch(self):
        srv = self.serve(lambda path, rid: response(rid, 200, b"hello\n"))
        out = self.bcurl("127.0.0.1:%d/hello.txt" % srv.port)
        self.assertEqual((out.returncode, out.stdout), (0, b"hello\n"))
        rid, method, path, headers = srv.requests[0]
        self.assertEqual((rid, method, path), (1, GET, "/hello.txt"))
        self.assertEqual(dict(headers)["host"], "127.0.0.1:%d" % srv.port)
        self.assertIn("user-agent", dict(headers))

    def test_skips_unknown_frames_everywhere(self):
        junk = frame(0x42, 0xFF, 0, b"v2 stuff") + frame(0xFE, 0, 7, os.urandom(40_000))
        srv = self.serve(lambda path, rid: response(
            rid, 200, chunks=[b"ab", b"cd"], headers=[hdr("content-length", "4")],
            extra_before=junk, extra_mid=junk))
        out = self.bcurl("127.0.0.1:%d/" % srv.port)
        self.assertEqual((out.returncode, out.stdout), (0, b"abcd"))

    def test_literal_and_unknown_index_response_headers(self):
        hs = [hdr("x-powered-by", "python"), hdr(0xC8, "ignored"), hdr("content-length", "2")]
        srv = self.serve(lambda path, rid: response(rid, 200, b"ok", headers=hs))
        out = self.bcurl("-v", "127.0.0.1:%d/" % srv.port)
        self.assertEqual((out.returncode, out.stdout), (0, b"ok"))
        self.assertIn(b"(literal) x-powered-by: python", out.stderr)
        self.assertIn(b"unknown index 200, ignored", out.stderr)

    def test_odd_data_splits_and_empty_final_frame(self):
        srv = self.serve(lambda path, rid: response(
            rid, 200, chunks=[b"", b"x" * 3, b"y" * 20_000, b""],
            headers=[hdr("content-length", "20003")]))
        out = self.bcurl("127.0.0.1:%d/" % srv.port)
        self.assertEqual((out.returncode, out.stdout), (0, b"xxx" + b"y" * 20_000))

    def test_exit_codes(self):
        statuses = {"/ok": 200, "/redirect": 301, "/missing": 404, "/boom": 500}
        srv = self.serve(lambda path, rid: response(rid, statuses[path], b"body"))
        base = "127.0.0.1:%d" % srv.port
        self.assertEqual(self.bcurl(base + "/ok").returncode, 0)
        self.assertEqual(self.bcurl(base + "/redirect").returncode, 0)
        self.assertEqual(self.bcurl(base + "/missing").returncode, 4)
        self.assertEqual(self.bcurl(base + "/boom").returncode, 5)
        self.assertEqual(self.bcurl(base + "/missing", base + "/boom", base + "/ok").returncode, 5)

    def test_many_urls_one_connection(self):
        srv = self.serve(lambda path, rid: response(rid, 200, path.encode()))
        base = "bhttp://127.0.0.1:%d" % srv.port
        out = self.bcurl(base + "/a", base + "/b", base + "/c")
        self.assertEqual((out.returncode, out.stdout), (0, b"/a/b/c"))
        self.assertEqual(srv.connections, 1)
        self.assertEqual([r[0] for r in srv.requests], [1, 2, 3])

    def test_refuses_two_hosts(self):
        self.assertEqual(self.bcurl("127.0.0.1:1/a", "127.0.0.1:2/b").returncode, 2)

    def test_head_and_custom_headers(self):
        srv = self.serve(lambda path, rid: frame(
            RESPONSE, END, rid, b"\x00\xc8" + hdr("content-length", "999")))
        out = self.bcurl("-I", "-H", "X-Trace: abc", "-H", "Accept: text/html",
                         "127.0.0.1:%d/" % srv.port)
        self.assertEqual((out.returncode, out.stdout), (0, b""))
        _, method, _, headers = srv.requests[0]
        self.assertEqual(method, HEAD)
        self.assertIn(("x-trace", "abc"), headers)
        self.assertEqual([v for n, v in headers if n == "accept"], ["text/html"])

    def test_verbose_hexdumps_to_stderr_only(self):
        srv = self.serve(lambda path, rid: response(rid, 200, b"hi"))
        out = self.bcurl("-v", "127.0.0.1:%d/x" % srv.port)
        self.assertEqual(out.stdout, b"hi")
        for marker in (b"> preface", b"> REQUEST id=1", b"< RESPONSE id=1", b"< DATA id=1",
                       b"> GOAWAY", b"42 48 54 54 50 2f 31 0a"):
            self.assertIn(marker, out.stderr)

    def test_protocol_errors_exit_1(self):
        cases = {
            "wrong id": lambda path, rid: response(rid + 1, 200, b"x"),
            "data before response": lambda path, rid: frame(DATA, END, rid, b"x"),
            "short body": lambda path, rid: response(rid, 200, b"abc",
                                                     headers=[hdr("content-length", "10")]),
            "goaway": lambda path, rid: frame(GOAWAY, 0, 0, b"\x00\x01nope"),
            "close": lambda path, rid: b"",
        }
        for name, respond in cases.items():
            with self.subTest(case=name):
                srv = self.serve(respond)
                if name == "close":
                    srv.respond = lambda path, rid: (_ for _ in ()).throw(EOFError())
                self.assertEqual(self.bcurl("127.0.0.1:%d/" % srv.port).returncode, 1)


if __name__ == "__main__":
    unittest.main()
