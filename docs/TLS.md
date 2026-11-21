# Serving HTTPS directly

Include `<webfront/tls/WebFront.hpp>` and use `webfront::tls::WebFront<>` for a
TLS-only listener. It serves HTTP/1.1 and the WebFront bridge over WSS on the same port.
The component is header-only and links without a TLS library.

> The TLS profile is proposed and its constant-time behavior has not been audited.
> Limit access to controlled networks while it is being qualified. For an
> Internet-facing listener, use a deployment proxy with connection admission
> controls and handshake deadlines.

```cpp
#include <webfront/tls/WebFront.hpp>

int main() {
    auto credentials = std::make_shared<webfront::tls::Credentials>(
        "/path/to/fullchain.pem", "/path/to/privkey.pem");
    webfront::tls::WebFront app({
        .address = "127.0.0.1",
        .port = "8443",
        .contentPolicy = {.allowMissingOrigin = false},
        .tlsCredentials = credentials,
    });
    app.cppFunction("greeting", [] { return std::string("Hello over HTTPS"); });
    app.run();
}
```

Use an ECDSA P-256 leaf certificate whose SAN covers the DNS name users visit.
Supply the PEM chain in leaf-first order and an unencrypted PEM private key
as a single `PRIVATE KEY` (PKCS#8) or `EC PRIVATE KEY` (SEC1) block. The files
accept whitespace around the PEM blocks. Extra text such as `Bag Attributes`
and separate `EC PARAMETERS` blocks are refused. To generate a single SEC1
block, use `openssl ecparam -name prime256v1 -genkey -noout -out privkey.pem`.
Give the application account read access to the key.
WebFront checks the structure of each certificate and that the leaf matches the
private key, but not validity dates or the chain's trust: the browser does.
After the certificate manager has written both renewed files, call
`credentials->reload(fullchain, privkey)` from the application's renewal hook.
It checks that the leaf and key match before publishing them to new handshakes.
A failed reload throws and preserves the active identity. Existing connections
continue with their negotiated keys.

Open the deployment URL in the browser. When using `show()` on a local bind,
the certificate must cover the configured address used in the window URL.
The browser verifies the certificate chain and server name. WebFront applies
the usual bridge origin policy. The file-service binding cookie carries `Secure`.
File reads for non-associated clients require an explicit `remoteFileTransport`
declaration, typically `httpsByDeployment`. The default `refused` applies to
TLS listeners as well. Client authentication and authorization belong to the
application. A non-browser client can supply its own `Origin` header. The
example requires the header so requests without one are refused.

The initial profile negotiates TLS 1.3, X25519, ECDSA P-256 with SHA-256 and
`TLS_CHACHA20_POLY1305_SHA256`. Clients must offer this suite. The profile
uses full handshakes and HTTP/1.1. If a client sends ALPN, it must offer
`http/1.1`. An ALPN offer without it is rejected with `no_application_protocol`
(alert 120). Where clients require AES-GCM, HTTP/2,
or another certificate type, terminate TLS at the deployment proxy and
configure `httpsByDeployment` if file-service reads are enabled.
Client 0-RTT ciphertext offered with a PSK is discarded, up to 64 KiB.

The handshake parser limits ClientHello to 64 KiB and 128 extensions, and the
receive buffer to 16,640 bytes of ciphertext. Incoming handshake messages may
span records, with one message at a time. The server supports the ClientHello,
optional second ClientHello and Finished sequence described in the
[decision record](adr/ADR-024-tls-integre.md).

Each listener admits up to 16 concurrent TLS handshakes, with a ten-second
deadline from accept through Finished. When all slots are occupied, a newly
accepted socket is closed immediately. Established connections release their
slot. The two X25519 operations and the CertificateVerify signature run on
the I/O thread. A sustained rate of incoming handshakes can saturate that
thread and cause denial of service, including for established HTTP/WSS
connections. The limit of 16 bounds the work in progress, not an attacker's
reach: sixteen idle connections keep every slot for up to ten seconds and can be
renewed, so it is not a defence against denial of service. Put a rate limiter or
a firewall in front of an exposed listener. WebFront writes a warning to standard error when the integrated
listener is configured outside loopback. Enable
`log::Debug` with a log sink to see handshake failure reasons and error codes.

On orderly WSS closure initiated by the peer, WebFront answers the WebSocket
Close frame and then sends TLS `close_notify`. The secure queue retains the
active frame and discards other queued frames and subsequent writes once the
peer's Close is accepted. The TCP queue follows its existing stop behavior.
If TLS closure is requested during a send, the alert follows the current
complete TLS record. Local `stop()` during a write can cancel that record,
so the peer sees a truncated TLS stream without `close_notify`. A reset or
failed write can also prevent the alert from being sent.

A peer's `KeyUpdate(update_requested)` is answered before the next
application data record. The response stays pending while the server is idle.

When using `tls::Server` directly, run handshakes for each acceptor on its I/O
thread and serialize `Send` calls on each socket. HTTP and WebSocket enforce
this contract when using `tls::WebFront`.

## Constant-time regression

On Linux with GCC or Clang, Valgrind and its development headers installed, the optional
`TLSConstantTime` CTest case marks the private inputs to `p256Base` and `x25519`
undefined and checks compiler-generated branches and addresses with Memcheck.
The probe passed locally on x86-64 with GCC 16.1 and Clang 21 at `-Os` and `-O3`.
The probe covers those scalar operations and compiler configurations. It leaves
an audit of the rest of TLS and other targets, including Thumb-2, outstanding.

## Size, memory and handshake time

On Linux x86-64 with GCC 16.1, a minimal app with one bridge callback built at
`-Os -DNDEBUG -ffunction-sections -fdata-sections -Wl,--gc-sections` gained
47.3 KiB of `.text` and 5.5 KiB of `.rodata` when the TCP provider was
replaced by the TLS provider. The TLS headers total about 1,500 lines, including the in-tree primitives and comments.

In the same binaries, 200 open loopback WebSockets increased Linux
`smaps_rollup` private dirty memory by about 3.5 KiB more per idle connection
for WSS than for WS. Giving each socket 16,384 bytes of an incomplete frame
increased the WSS overhead to about 18.8 KiB per connection. These are process
measurements after a warm-up connection and include allocator overhead.
Thumb-2 size and peak memory during the handshake remain unmeasured.

On an AMD Ryzen 9 3900X, an OpenSSL-backed Python client completed 200
sequential loopback handshakes in 2.0 ms at the median and 2.2 ms at the 95th
percentile. The measurement starts after TCP connect and ends after TLS
negotiation, following ten warm-up connections. It includes both endpoints
and excludes HTTP traffic.
