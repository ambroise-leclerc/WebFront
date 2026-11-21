# Serving HTTPS directly

Include `<webfront/tls/WebFront.hpp>` and use `webfront::tls::WebFront<>` for a
TLS-only listener. It serves HTTP/1.1 and the WebFront bridge over WSS on the same port.
The component is header-only and links without a TLS library.

```cpp
#include <webfront/tls/WebFront.hpp>

int main() {
    auto credentials = std::make_shared<webfront::tls::Credentials>(
        "/path/to/fullchain.pem", "/path/to/privkey.pem");
    webfront::tls::WebFront app({
        .address = "0.0.0.0",
        .port = "8443",
        .contentPolicy = {.bridgeAllowedOrigins = {"https://app.example.com:8443"}},
        .tlsCredentials = credentials,
    });
    app.cppFunction("greeting", [] { return std::string("Hello over HTTPS"); });
    app.run();
}
```

Use an ECDSA P-256 leaf certificate whose SAN covers the DNS name users visit.
Supply the PEM chain in leaf-first order and an unencrypted PEM private key
in PKCS#8 or SEC1 form. Give the application account read access to the key.
After the certificate manager has written both renewed files, call
`credentials->reload(fullchain, privkey)` from the application's renewal hook.
It checks that the leaf and key match before publishing them to new handshakes.
A failed reload throws and preserves the active identity. Existing connections
continue with their negotiated keys.

Open the deployment URL in the browser. When using `show()` on a local bind,
the certificate must cover the configured address used in the window URL.
The browser verifies the certificate chain and server name. WebFront applies
the usual bridge origin policy, and the file-service binding cookie has the
`Secure` attribute on the TLS listener. Client authentication and authorization
belong to the application.

The initial profile negotiates TLS 1.3, X25519, ECDSA P-256 with SHA-256 and
`TLS_CHACHA20_POLY1305_SHA256`. Clients must offer this suite. The profile
uses full handshakes and HTTP/1.1. Where clients require AES-GCM, HTTP/2,
or another certificate type, terminate TLS at the deployment proxy and
configure `httpsByDeployment` if file-service reads are enabled.

The handshake parser limits ClientHello to 64 KiB and the receive buffer to
16,640 bytes of ciphertext. Connection admission and idle timeouts are
deployment concerns. The [decision record](adr/ADR-024-tls-integre.md) describes
the component's scope and qualification.

## Size and memory

On Linux x86-64 with GCC 16.1, a minimal app with one bridge callback built at
`-Os -DNDEBUG -ffunction-sections -fdata-sections -Wl,--gc-sections` gained
43.6 KiB of `.text` and 5.0 KiB of `.rodata` when the TCP provider was
replaced by the TLS provider. The TLS headers total about 1,500 lines, including the in-tree primitives and comments.

In the same binaries, 200 open loopback WebSockets increased Linux
`smaps_rollup` private dirty memory by about 3.6 KiB more per idle connection
for WSS than for WS. Giving each socket 16,384 bytes of an incomplete frame
increased the WSS overhead to about 18.8 KiB per connection. These are process
measurements after a warm-up connection and include allocator overhead.
Thumb-2 size and peak memory during the handshake remain unmeasured.
