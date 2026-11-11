#!/usr/bin/env python3
"""The other end of the wire for tests/crnet_test.c.

Everything CrNet's test talks to, on this machine, written against Python's
standard library and nothing of CrNet's - so that what the test checks is
CrNet against an independent implementation, not against itself.

It serves, each on a port the system picks:

  http    a plain HTTP server                         (paths below)
  https   the same, behind a self-signed certificate made at startup
  ws      a WebSocket server
  wss     the same, behind the same certificate
  proxy   an HTTP proxy (absolute-URI requests and CONNECT tunnels), which
          takes every host it is asked for to be this machine

and writes what the test needs to find them as `name=value` lines to the file
named on the command line, once everything is listening:

  http_port, https_port, ws_port, wss_port, proxy_port
  cert_der     path of the server certificate, DER
  pin          SHA-256 of its SubjectPublicKeyInfo, hex

HTTP paths:

  /hello        200 "hello", with a header sent on two lines (X-Multi: a / b)
  /headers      200, the request's own headers back as "name: value" lines
  /echo         the request body back, whatever the method
  /big?n=N      N bytes of a repeating pattern, as fast as the client takes them
  /stat         "name=value" lines: connections accepted, bytes of the last
                /big written so far, requests the proxy has carried
  /redirect     302 to /hello
  /gzip         a gzip-compressed body, unless the request said
                Accept-Encoding: identity; X-Got-Accept-Encoding says what it said
  /slow?ms=N    waits N milliseconds, then 200 "slow"
  /early        answers 413 at once, without reading the request body; to a
                request with Expect: 100-continue, in place of the 100
  /empty        204, no body

WebSocket: every text or binary message is sent back as it came, except text
messages that are commands:

  "send:N"      the server sends one binary message of N bytes
  "close-me"    the server closes the connection with code 4001
  "burst:N"     the server sends N small text messages one after another

and the path /redirect answers the opening request with a 302 to /echo.

Usage: crnet_test_server.py <info file> <work dir>
"""

import base64
import gzip
import hashlib
import http.server
import os
import select
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.parse

STATE = {"connections": 0, "big_written": 0, "proxy_requests": 0}
STATE_LOCK = threading.Lock()


def bump(name, amount=1):
    with STATE_LOCK:
        STATE[name] += amount


def pattern(count, offset=0):
    """`count` bytes whose value depends only on their position."""
    block = bytes((i * 7 + 3) & 0xFF for i in range(256))
    start = offset % 256
    whole = block[start:] + block * (count // 256 + 1)
    return whole[:count]


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # connections are kept open between requests

    def log_message(self, *args):
        pass

    def handle_expect_100(self):
        # /early refuses in place of the 100, as a server refusing a body it
        # hasn't read does; everything else is told to go on.
        if urllib.parse.urlparse(self.path).path == "/early":
            self.close_connection = True
            self.reply(413, b"too much", [("Connection", "close")])
            return False
        return super().handle_expect_100()

    def read_body(self):
        """The request body, sized or chunked."""
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = b""
            while True:
                line = self.rfile.readline().strip()
                size = int(line.split(b";")[0], 16)
                if size == 0:
                    # Trailers, then the blank line.
                    while self.rfile.readline().strip():
                        pass
                    return body
                body += self.rfile.read(size)
                self.rfile.readline()
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length) if length else b""

    def reply(self, status, body=b"", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def handle_any(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        path = parsed.path

        if path == "/early":
            # Answered without the body being read, and the connection ended.
            self.close_connection = True
            self.reply(413, b"too much", [("Connection", "close")])
            return

        body = self.read_body()

        if path == "/hello":
            self.reply(200, b"hello", [("X-Multi", "a"), ("X-Multi", "b"), ("Content-Type", "text/plain")])
        elif path == "/headers":
            lines = "".join("%s: %s\n" % (name, value) for name, value in self.headers.items())
            self.reply(200, lines.encode("utf-8"), [("X-Method", self.command), ("X-Body-Size", str(len(body)))])
        elif path == "/echo":
            self.reply(200, body, [("X-Method", self.command), ("X-Body-Size", str(len(body)))])
        elif path == "/big":
            count = int(query.get("n", ["0"])[0])
            with STATE_LOCK:
                STATE["big_written"] = 0
            self.send_response(200)
            self.send_header("Content-Length", str(count))
            self.end_headers()
            sent = 0
            while sent < count:
                piece = pattern(min(65536, count - sent), sent)
                self.wfile.write(piece)
                self.wfile.flush()
                sent += len(piece)
                with STATE_LOCK:
                    STATE["big_written"] = sent
        elif path == "/stat":
            with STATE_LOCK:
                lines = "".join("%s=%d\n" % item for item in STATE.items())
            self.reply(200, lines.encode("utf-8"))
        elif path == "/redirect":
            self.reply(302, b"", [("Location", "/hello")])
        elif path == "/gzip":
            asked = self.headers.get("Accept-Encoding", "")
            plain = pattern(20000)
            if asked.strip().lower() == "identity":
                self.reply(200, plain, [("X-Got-Accept-Encoding", asked)])
            else:
                self.reply(200, gzip.compress(plain), [("Content-Encoding", "gzip"), ("X-Got-Accept-Encoding", asked)])
        elif path == "/slow":
            time.sleep(int(query.get("ms", ["1000"])[0]) / 1000.0)
            self.reply(200, b"slow")
        elif path == "/empty":
            self.send_response(204)
            self.end_headers()
        else:
            self.reply(404, b"no such path")

    do_GET = do_HEAD = do_POST = do_PUT = do_PATCH = do_DELETE = handle_any


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def get_request(self):
        bump("connections")
        return super().get_request()

    def handle_error(self, request, client_address):
        pass  # a client that hangs up part way is part of the test


# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        piece = sock.recv(count - len(data))
        if not piece:
            raise ConnectionError("closed")
        data += piece
    return data


def ws_send(sock, opcode, payload):
    head = bytes([0x80 | opcode])
    size = len(payload)
    if size < 126:
        head += bytes([size])
    elif size < 65536:
        head += bytes([126]) + struct.pack(">H", size)
    else:
        head += bytes([127]) + struct.pack(">Q", size)
    sock.sendall(head + payload)


def ws_read(sock):
    """One frame: (fin, opcode, payload)."""
    first, second = recv_exact(sock, 2)
    size = second & 0x7F
    if size == 126:
        size = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if second & 0x80 else None
    payload = recv_exact(sock, size)
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return bool(first & 0x80), first & 0x0F, payload


def ws_connection(sock):
    try:
        request = b""
        while b"\r\n\r\n" not in request:
            piece = sock.recv(4096)
            if not piece:
                return
            request += piece
        lines = request.split(b"\r\n")
        path = lines[0].split(b" ")[1].decode("ascii")
        headers = {}
        for line in lines[1:]:
            if b":" in line:
                name, value = line.split(b":", 1)
                headers[name.strip().lower()] = value.strip()

        if path == "/redirect":
            sock.sendall(b"HTTP/1.1 302 Found\r\nLocation: /echo\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        if path == "/refuse":
            sock.sendall(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return

        accept = base64.b64encode(hashlib.sha1(headers[b"sec-websocket-key"] + WS_GUID).digest())
        sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")

        message = b""
        message_opcode = 0
        while True:
            fin, opcode, payload = ws_read(sock)
            if opcode == 0x8:  # close: answered in kind
                ws_send(sock, 0x8, payload[:2])
                return
            if opcode == 0x9:  # ping
                ws_send(sock, 0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode != 0:
                message_opcode = opcode
                message = b""
            message += payload
            if not fin:
                continue

            if message_opcode == 0x1 and message.startswith(b"send:"):
                ws_send(sock, 0x2, pattern(int(message[5:])))
            elif message_opcode == 0x1 and message.startswith(b"burst:"):
                for index in range(int(message[6:])):
                    ws_send(sock, 0x1, b"burst %d" % index)
            elif message_opcode == 0x1 and message == b"close-me":
                ws_send(sock, 0x8, struct.pack(">H", 4001) + b"as asked")
                # The client's answer, or its going away.
                try:
                    ws_read(sock)
                except Exception:
                    pass
                return
            else:
                ws_send(sock, message_opcode, message)
    except Exception:
        pass
    finally:
        try:
            sock.close()
        except Exception:
            pass


def ws_serve(listener, context):
    while True:
        sock, _ = listener.accept()
        if context:
            try:
                sock = context.wrap_socket(sock, server_side=True)
            except Exception:
                sock.close()
                continue
        threading.Thread(target=ws_connection, args=(sock,), daemon=True).start()


# ---------------------------------------------------------------------------
# Proxy
# ---------------------------------------------------------------------------


def pipe_both(a, b):
    """Carries bytes both ways until either side ends."""
    try:
        while True:
            ready, _, _ = select.select([a, b], [], [], 60)
            if not ready:
                return
            for source in ready:
                data = source.recv(65536)
                if not data:
                    return
                (b if source is a else a).sendall(data)
    except Exception:
        pass
    finally:
        for sock in (a, b):
            try:
                sock.close()
            except Exception:
                pass


def proxy_connection(client):
    try:
        request = b""
        while b"\r\n\r\n" not in request:
            piece = client.recv(4096)
            if not piece:
                client.close()
                return
            request += piece
        head, rest = request.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        method, target, version = lines[0].split(b" ", 2)
        bump("proxy_requests")

        # Whatever host the client named, the server it means is this
        # machine's: the test names a host that doesn't exist, so that a
        # request can only arrive if it really came through here.
        if method == b"CONNECT":
            host, port = target.rsplit(b":", 1)
            upstream = socket.create_connection(("127.0.0.1", int(port)))
            client.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            if rest:
                upstream.sendall(rest)
        else:
            parsed = urllib.parse.urlsplit(target.decode("ascii"))
            upstream = socket.create_connection(("127.0.0.1", parsed.port or 80))
            path = parsed.path or "/"
            if parsed.query:
                path += "?" + parsed.query
            kept = [line for line in lines[1:] if not line.lower().startswith(b"proxy-")]
            upstream.sendall(method + b" " + path.encode("ascii") + b" " + version + b"\r\n" + b"\r\n".join(kept) + b"\r\n\r\n" + rest)
        pipe_both(client, upstream)
    except Exception:
        try:
            client.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            client.close()
        except Exception:
            pass


def proxy_serve(listener):
    while True:
        sock, _ = listener.accept()
        threading.Thread(target=proxy_connection, args=(sock,), daemon=True).start()


# ---------------------------------------------------------------------------
# The certificate
# ---------------------------------------------------------------------------


def make_certificate(work):
    """A self-signed server certificate for the name "localhost" only - not
    for 127.0.0.1, so that the test can ask for a host-name mismatch - made
    the way Apple's systems insist a server certificate be: a SAN, the
    server-auth usage, a short life."""
    config = os.path.join(work, "cert.cnf")
    with open(config, "w") as out:
        out.write(
            "[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n"
            "[dn]\nCN=localhost\n"
            "[ext]\nsubjectAltName=DNS:localhost\nbasicConstraints=critical,CA:TRUE\n"
            "keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\nextendedKeyUsage=serverAuth\n"
        )
    key = os.path.join(work, "key.pem")
    cert = os.path.join(work, "cert.pem")
    der = os.path.join(work, "cert.der")
    quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "30", "-config", config, "-keyout", key, "-out", cert], **quiet)
    subprocess.check_call(["openssl", "x509", "-in", cert, "-outform", "der", "-out", der], **quiet)
    public = subprocess.check_output(["openssl", "x509", "-in", cert, "-pubkey", "-noout"], stderr=subprocess.DEVNULL)
    info = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=public, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True).stdout
    return cert, key, der, hashlib.sha256(info).hexdigest()


def main():
    info_path, work = sys.argv[1], sys.argv[2]
    cert, key, der, pin = make_certificate(work)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)

    plain = Server(("127.0.0.1", 0), Handler)
    secure = Server(("127.0.0.1", 0), Handler)
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    threading.Thread(target=plain.serve_forever, daemon=True).start()
    threading.Thread(target=secure.serve_forever, daemon=True).start()

    def listen():
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(32)
        return listener

    ws = listen()
    wss = listen()
    proxy = listen()
    threading.Thread(target=ws_serve, args=(ws, None), daemon=True).start()
    threading.Thread(target=ws_serve, args=(wss, context), daemon=True).start()
    threading.Thread(target=proxy_serve, args=(proxy,), daemon=True).start()

    # Written whole and then moved into place, so the test never reads half.
    with open(info_path + ".part", "w") as out:
        out.write("http_port=%d\n" % plain.server_address[1])
        out.write("https_port=%d\n" % secure.server_address[1])
        out.write("ws_port=%d\n" % ws.getsockname()[1])
        out.write("wss_port=%d\n" % wss.getsockname()[1])
        out.write("proxy_port=%d\n" % proxy.getsockname()[1])
        out.write("cert_der=%s\n" % der)
        out.write("pin=%s\n" % pin)
    os.replace(info_path + ".part", info_path)

    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
