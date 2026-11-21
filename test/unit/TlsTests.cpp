#include "Bdd.hpp"
#include <webfront/tls/Credentials.hpp>
#include <webfront/tls/detail/Protocol.hpp>

using namespace webfront::bdd;

namespace {
namespace tls = webfront::tls::detail;
using tls::Bytes;
using tls::View;

Bytes tlsHex(std::string_view text) {
    constexpr std::string_view digits = "0123456789abcdef";
    Bytes                      result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<tls::Byte>(digits.find(text[i]) * 16 + digits.find(text[i + 1])));
    return result;
}

Bytes traceHello() {
    return tlsHex("010000c00303cb34ecb1e78163ba1c38c6dacb196a6dffa21a8d9912ec18a2ef6283024dece70000061301130313020100"
                  "00910000000b0009000006736572766572ff01000100000a00140012001d0017001800190100010101020103010400230000"
                  "003300260024001d002099381de560e4bd43d23d8e435a7dbafeb3c06e51c13cae4d5413691e529aaf2c002b0003020304"
                  "000d0020001e040305030603020308040805080604010501060102010402050206020202002d00020101001c00024001");
}

const Scenario tlsPrimitives("TLS cryptographic vectors", {"fast"}, [] {
    then("SHA-256 matches the standard message digest", [] {
        require(std::ranges::equal(tls::hash(tls::bytes("abc")), tlsHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
    });
    then("HKDF extract and expand match RFC 5869 A.1", [] {
        const auto  salt = tlsHex("000102030405060708090a0b0c");
        const auto  info = tlsHex("f0f1f2f3f4f5f6f7f8f9");
        const Bytes input(22, 0x0b);
        const auto  key = tls::hmac(salt, {input});
        require(std::ranges::equal(key, tlsHex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5")));
        const auto first    = tls::hmac(key, {info, std::array<tls::Byte, 1>{1}});
        const auto second   = tls::hmac(key, {first, info, std::array<tls::Byte, 1>{2}});
        const auto expected = tlsHex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
        require(std::ranges::equal(first, View(expected).first(32)));
        require(std::ranges::equal(View(second).first(10), View(expected).subspan(32)));
    });
    then("X25519 matches RFC 7748 section 6.1", [] {
        const auto  scalar = tlsHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        const auto  peer   = tlsHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
        tls::Digest output;
        tls::mono::crypto_x25519_public_key(output.data(), scalar.data());
        require(std::ranges::equal(output, tlsHex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")));
        tls::mono::crypto_x25519(output.data(), scalar.data(), peer.data());
        require(std::ranges::equal(output, tlsHex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")));
    });
    then("the P-256 signature matches RFC 6979 A.2.5", [] {
        const auto scalar = tlsHex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
        require(tls::sign(scalar, tls::hash(tls::bytes("sample")))
                == tlsHex("3046022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                          "022100f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"));
    });
    then("ChaCha20-Poly1305 matches RFC 8439 section 2.8.2", [] {
        const auto key     = tlsHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        const auto nonce   = tlsHex("070000004041424344454647");
        const auto aad     = tlsHex("50515253c0c1c2c3c4c5c6c7");
        const auto message = tls::bytes("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
        Bytes      ciphertext(message.size());
        std::array<tls::Byte, 16>  tag;
        tls::mono::crypto_aead_ctx context;
        tls::mono::crypto_aead_init_ietf(&context, key.data(), nonce.data());
        tls::mono::crypto_aead_write(&context, ciphertext.data(), tag.data(), aad.data(), aad.size(), message.data(), message.size());
        require(ciphertext
                == tlsHex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
                          "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                          "3ff4def08e4b7a9de576d26586cec64b6116"));
        require(std::ranges::equal(tag, tlsHex("1ae10b594f09e26a7e902ecbd0600691")));
    });
});

const Scenario tlsHandshakeTrace("TLS 1.3 handshake against RFC 8448", {"fast"}, [] {
    const auto hello  = traceHello();
    const auto server = tlsHex("020000560303a6af06a4121860dc5e6e60249cd34c95930c8ac5cb1434dac155772ed3e2692800130100002e"
                               "00330024001d0020c9828876112095fe66762bdbf7c672e156d6cc253b833df1dd69b1b04e751f0f002b00020304");
    given("the client and server hellos from section 3", [&] {
        const auto  parsed = tls::parseHello(hello, false);
        const auto  scalar = tlsHex("b1580eeadf6dd589b8ef4f2d5652578cc810e9980191ec8d058308cea216a21e");
        tls::Digest shared;
        tls::mono::crypto_x25519(shared.data(), scalar.data(), parsed.share.data());
        tls::Sha256 transcript;
        transcript.update(hello);
        transcript.update(server);
        then("the handshake traffic secrets match the published trace", [&] {
            const auto early  = tls::hmac(tls::zeros, {tls::zeros});
            const auto salt   = tls::expandLabel(early, "derived", tls::hash());
            const auto secret = tls::hmac(salt, {shared});
            require(std::ranges::equal(tls::expandLabel(secret, "c hs traffic", transcript.digest()),
                                       tlsHex("b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21")));
            require(std::ranges::equal(tls::expandLabel(secret, "s hs traffic", transcript.digest()),
                                       tlsHex("b67b7d690cc16c4e75e54213cb2d37b4e9c912bcded9105d42befd59d391ad38")));
        });
    });
});

const Scenario tlsRefusals("TLS rejects invalid framing and negotiation", {"fast"}, [] {
    then("every truncated ClientHello is refused", [] {
        const auto hello = traceHello();
        for (std::size_t size = 0; size < hello.size(); ++size)
            requireThrows<tls::Error>([&] {
                tls::parseHello(View(hello).first(size), false);
            });
    });
    then("unsupported versions and cipher suites are refused", [] {
        auto hello = traceHello();
        hello[5]   = 2;
        requireThrows<tls::Error>([&] {
            tls::parseHello(hello, false);
        });
        hello[5]  = 3;
        hello[44] = 2;  // TLS_AES_256_GCM_SHA384 instead of ChaCha20-Poly1305.
        requireThrows<tls::Error>([&] {
            tls::parseHello(hello, false);
        });
    });
    then("oversized records and corrupted authentication tags are refused", [] {
        requireThrows<tls::Error>([] {
            tls::parseRecord(tlsHex("1703034101"), true);
        });
        tls::Traffic sender, receiver;
        sender.install(tls::zeros);
        receiver.install(tls::zeros);
        Bytes record{0, 0, 0, 0, 0, 0x70};
        sender.seal(tls::applicationData, record);
        Bytes payload(record.begin() + 5, record.end());
        payload.back() ^= 1;
        requireThrows<tls::Error>([&] {
            receiver.open(View(record).first(5), payload);
        });
    });
});
}  // namespace
