"""Exercise the TLS provider against an independent OpenSSL client over loopback."""
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

    context = ssl.create_default_context(cafile=certificate)
    context.minimum_version = ssl.TLSVersion.TLSv1_3

    def connect():
        return context.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5),
                                   server_hostname="127.0.0.1")

    post = b"POST /post/ HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n"
    with connect() as peer:
        peer.sendall(post + b"x")
        with peer.unwrap():
            pass  # close_notify is acknowledged even though the HTTP body is incomplete

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
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as server:
        try:
            exercise(server, openssl, certificate)
        finally:
            server.stdin.close()
            if server.wait(timeout=5) != 0:
                raise RuntimeError("TLS interop server exited unsuccessfully")
