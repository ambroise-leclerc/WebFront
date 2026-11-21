"""Exercise the TLS provider against an independent OpenSSL client over loopback."""
import errno
import re
import select
import socket
import ssl
import struct
import subprocess
import sys
from pathlib import Path


def exercise(server, openssl, certificate):
    port = int(server.stdout.readline())
    request = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    retry = subprocess.run(
        [openssl, "s_client", "-connect", f"127.0.0.1:{port}", "-tls1_3",
         "-groups", "P-256:X25519", "-CAfile", str(certificate), "-verify_return_error",
         "-verify_ip", "127.0.0.1", "-quiet", "-msg"],
        input=request, capture_output=True, check=True, timeout=10,
    )
    if retry.stdout.count(b", ServerHello") != 2 or b"HTTP/1.1 200 OK" not in retry.stdout:
        raise RuntimeError(f"HelloRetryRequest or HTTP response missing: {retry.stdout!r} {retry.stderr!r}")

    with socket.create_connection(("127.0.0.1", port), timeout=13) as stalled:
        stalled.sendall(b"\x16\x03")
        if stalled.recv(1) != b"":
            raise RuntimeError("Incomplete TLS record should be closed at the handshake deadline")

    context = ssl.create_default_context(cafile=certificate)
    context.minimum_version = ssl.TLSVersion.TLSv1_3

    def connect():
        return context.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5),
                                   server_hostname="127.0.0.1", suppress_ragged_eofs=False)

    context.set_alpn_protocols(["h2"])
    try:
        with connect():
            raise RuntimeError("A client offering only h2 in ALPN should be refused")
    except ssl.SSLError as error:
        if error.reason != "TLSV1_ALERT_NO_APPLICATION_PROTOCOL":
            raise
    context.set_alpn_protocols(["http/1.1"])

    # A WebSocket Close must be answered before the authenticated TLS EOF.
    with connect() as peer:
        if peer.selected_alpn_protocol() != "http/1.1":
            raise RuntimeError("The server did not negotiate HTTP/1.1 in ALPN")
        peer.sendall(
            f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
            f"Origin: https://127.0.0.1:{port}\r\n"
            "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: WebFront_0.1\r\n\r\n".encode()
        )
        with peer.makefile("rb") as response:
            if response.readline().rstrip() != b"HTTP/1.1 101 Switching Protocols":
                raise RuntimeError("WebSocket upgrade failed")
            while response.readline() not in (b"\r\n", b""):
                pass
            peer.sendall(bytes.fromhex("88820102030402ea"))  # masked status 1000
            if response.read(4) != bytes.fromhex("880203e8") or response.read(1):
                raise RuntimeError("Missing WebSocket Close reply or TLS close_notify")

    post = b"POST /post/ HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n"
    with connect() as peer:
        peer.sendall(post + b"x")
        with peer.unwrap():
            pass  # close_notify is acknowledged even though the HTTP body is incomplete

    with connect() as peer:
        peer.sendall(post + b"x")
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))

    with connect() as peer:
        peer.sendall(post + b"ok")
        if not select.select([server.stdout], [], [], 5)[0] or server.stdout.readline().strip() != "post":
            raise RuntimeError("The server did not enter the POST handler")
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    # Release the handler only once the client has reset its socket.
    server.stdin.write("\n")
    server.stdin.flush()
    with connect() as peer:
        peer.sendall(request)
        with peer.makefile("rb") as response:
            if response.readline().rstrip() != b"HTTP/1.1 200 OK":
                raise RuntimeError("The listener did not serve the next connection")


if __name__ == "__main__":
    program, openssl, fixtures = sys.argv[1:]
    certificate = Path(fixtures) / "cert.pem"
    with subprocess.Popen([program, certificate, Path(fixtures) / "key.pem"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as server:
        try:
            exercise(server, openssl, certificate)
        finally:
            server.stdin.close()
            if server.wait(timeout=5) != 0:
                raise RuntimeError("TLS interop server exited unsuccessfully")
        diagnostics = server.stderr.read()
        if not re.search(r"TLS handshake failed \([^:\n]+:\d+\): TLS: HTTP/1\.1 not offered", diagnostics):
            raise RuntimeError(f"Missing ALPN handshake diagnostic: {diagnostics!r}")
        if not re.search(rf"TLS handshake failed \([^:\n]+:{errno.ETIMEDOUT}\): ", diagnostics):
            raise RuntimeError(f"Missing handshake timeout diagnostic: {diagnostics!r}")
