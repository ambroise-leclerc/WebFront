// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-21
//
// WebFront — tests/unit
// Role: The TLS implementation: primitives, handshake parsing and records against known answers.
// Thread: bridge-io | Alloc: heap-init
// LLM: known-answer tests

#include "TemporaryDirectory.hpp"
#include <webfront/http/Encodings.hpp>
#include <webfront/testing/Bdd.hpp>
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
        const auto scalar = tlsHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        const auto peer   = tlsHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
        const auto secret = View(scalar).first<32>();
        auto       output = tls::x25519(secret, tls::Digest{9});
        require(std::ranges::equal(output, tlsHex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")));
        output = tls::x25519(secret, View(peer).first<32>());
        require(std::ranges::equal(output, tlsHex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")));
    });
    then("the P-256 signature matches RFC 6979 A.2.5", [] {
        const auto scalar = tlsHex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
        require(tls::sign(View(scalar).first<32>(), tls::hash(tls::bytes("sample")))
                == tlsHex("3046022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                          "022100f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"));
    });
    then("ChaCha20-Poly1305 matches RFC 8439 section 2.8.2", [] {
        const auto  key     = tlsHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        const auto  nonce   = tlsHex("070000004041424344454647");
        const auto  aad     = tlsHex("50515253c0c1c2c3c4c5c6c7");
        const auto  message = tls::bytes("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
        tls::Digest secret;
        tls::Nonce  iv;
        std::ranges::copy(key, secret.begin());
        std::ranges::copy(nonce, iv.begin());
        Bytes ciphertext(message.begin(), message.end());
        tls::chachaXor(secret, iv, ciphertext);
        const auto tag = tls::polyTag(secret, iv, aad, ciphertext);
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
        const auto  shared = tls::x25519(View(scalar).first<32>(), parsed.share.first<32>());
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
    then("a missing signature_algorithms extension elicits missing_extension", [] {
        auto       hello     = traceHello();
        const auto extension = std::ranges::search(hello, tlsHex("000d0020001e"));
        require(!extension.empty());
        *extension.begin() = 0xfe;  // Unknown extension of the same length.
        try {
            tls::parseHello(hello, false);
            require(false);
        } catch (const tls::Error& error) {
            require(error.alert == 109);
        }
    });
    then("a PSK offer requires key-exchange modes, and early data is forbidden on retry", [] {
        auto hello = traceHello();
        // Remove the modes extension from the trace and append an early-data PSK offer.
        const auto modes = tlsHex("002d00020101");
        const auto match = std::ranges::search(hello, modes);
        require(!match.empty());
        hello.erase(match.begin(), match.end());
        tls::append(hello, tlsHex("002a00000029002c000700010100000000002120"));
        hello.resize(hello.size() + 32);
        const auto lengths = [&] {
            hello[3]  = static_cast<tls::Byte>(hello.size() - 4);
            hello[50] = static_cast<tls::Byte>(hello.size() - 51);
        };
        lengths();
        requireThrows<tls::Error>(
            [&] {
                tls::parseHello(hello, false);
            },
            contains("missing psk_key_exchange_modes"));
        hello.insert(hello.begin() + 51, modes.begin(), modes.end());
        lengths();
        require(tls::parseHello(hello, false).earlyData);
        requireThrows<tls::Error>(
            [&] {
                tls::parseHello(hello, true);
            },
            contains("unexpected early_data"));
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
        payload.back() ^= 1;
        require(receiver.open(View(record).first(5), payload) == tls::applicationData);
    });
    then("padding after the inner type is accepted, and the content limit applies once it is removed", [] {
        const auto key = tls::expandLabel(tls::zeros, "key");
        tls::Nonce nonce;
        std::ranges::copy_n(tls::expandLabel(tls::zeros, "iv", {}, 12).begin(), nonce.size(), nonce.begin());
        const auto open = [&](std::size_t content, std::size_t padding, Bytes& payload) {
            payload.assign(content + 1 + padding, 0);
            payload[content] = tls::applicationData;
            Bytes header{tls::applicationData, 3, 3};
            tls::put(header, payload.size() + 16, 2);
            tls::chachaXor(key, nonce, payload);
            tls::append(payload, tls::polyTag(key, nonce, header, payload));
            tls::Traffic receiver;
            receiver.install(tls::zeros);
            return receiver.open(header, payload);
        };
        Bytes payload;
        require(open(tls::maxPlaintext, 0, payload) == tls::applicationData);
        require(payload.size() == tls::maxPlaintext);
        require(open(tls::maxPlaintext, 1, payload) == tls::applicationData);  // a full record with one byte of padding
        require(payload.size() == tls::maxPlaintext);
        require(open(10, 200, payload) == tls::applicationData);
        require(payload.size() == 10);
        requireThrows<tls::Error>(
            [&] {
                open(tls::maxPlaintext + 1, 0, payload);
            },
            contains("oversized inner plaintext"));
        requireThrows<tls::Error>(
            [&] {
                open(tls::maxPlaintext + 1, 1, payload);
            },
            contains("oversized inner plaintext"));
    });
    then("an exhausted record sequence fails as a transport error before wrapping", [] {
        require(tls::recordNonce({}, UINT64_MAX - 1).back() == 0xfe);
        try {
            tls::recordNonce({}, UINT64_MAX);
            require(false);
        } catch (const std::system_error& error) {
            require(error.code() == std::errc::value_too_large);
        }
    });
});
}  // namespace

namespace {
const Scenario tlsRecordControl("TLS application records coordinate updates and closure", [] {
    tls::RecordControl control;
    tls::Traffic       traffic;
    tls::Secret<>      secret{};
    traffic.install(secret);
    given("a KeyUpdate split across authenticated handshake records", [&] {
        require(!control.receive(tls::handshakeRecord, tlsHex("180000"), traffic, secret));
        require(!control.updateRequested);
        const auto next = tls::expandLabel(secret, "traffic upd");
        require(!control.receive(tls::handshakeRecord, tlsHex("0101"), traffic, secret));
        require(control.updateRequested);
        require(std::ranges::equal(secret, next));
        tls::Traffic sender;
        sender.install(next);
        Bytes record{0, 0, 0, 0, 0, 42};
        sender.seal(tls::applicationData, record);
        Bytes ciphertext(record.begin() + 5, record.end());
        require(traffic.open(View(record).first(5), ciphertext) == tls::applicationData);
        require(control.receive(tls::applicationData, ciphertext, traffic, secret));
    });
    then("an unexpected handshake type elicits unexpected_message", [&] {
        try {
            control.receive(tls::handshakeRecord, tlsHex("14000000"), traffic, secret);
            require(false);
        } catch (const tls::Error& error) {
            require(error.alert == 10);
        }
    });
    then("32 empty records are tolerated and application data resets the count", [&] {
        tls::RecordControl limit;
        for (unsigned batch = 0; batch != 2; ++batch) {
            for (unsigned i = 0; i != 32; ++i)
                require(!limit.receive(tls::applicationData, {}, traffic, secret));
            if (batch == 0)
                require(limit.receive(tls::applicationData, tlsHex("42"), traffic, secret));
        }
        requireThrows<tls::Error>([&] {
            limit.receive(tls::applicationData, {}, traffic, secret);
        });
    });
    then("the orderly response is sent once after any in-flight write", [&] {
        for (const bool sending : {false, true}) {
            tls::RecordControl closing;
            closing.sending = sending;
            require(!closing.receive(tls::alert, tlsHex("0100"), traffic, secret));
            require(closing.peerNotified);
            require(closing.beginClose() == !sending);
            require(closing.closed);
            require(!closing.beginClose());
            // The active writer consumes the alert once it has finished its record.
            require(closing.takeAlert() == 0);
            require(!closing.takeAlert());
        }
    });
    then("a transport reset discards a pending alert and prohibits new writes", [&] {
        tls::RecordControl reset;
        reset.sending = true;
        require(!reset.beginClose());
        reset.transportLost();
        require(reset.closed);
        require(!reset.takeAlert());
    });
    then("user_canceled allows the peer to follow with close_notify", [&] {
        tls::RecordControl warning;
        require(!warning.receive(tls::alert, tlsHex("015a"), traffic, secret));
        require(!warning.closed);
        require(!warning.receive(tls::alert, tlsHex("0100"), traffic, secret));
        require(warning.peerNotified);
    });
    then("close_notify cannot interrupt a fragmented KeyUpdate", [&] {
        tls::RecordControl fragmented;
        require(!fragmented.receive(tls::handshakeRecord, tlsHex("18"), traffic, secret));
        requireThrows<tls::Error>([&] {
            fragmented.receive(tls::alert, tlsHex("0100"), traffic, secret);
        });
        require(!fragmented.peerNotified);
    });
    then("a fatal peer alert closes without a reply", [&] {
        tls::RecordControl fatal;
        requireThrows<std::system_error>([&] {
            fatal.receive(tls::alert, tlsHex("0214"), traffic, secret);
        });
        require(fatal.closed);
        require(!fatal.beginClose());
        require(!fatal.takeAlert());
    });
});

const Scenario tlsPolyBoundary("Poly1305 reduction handles a carry around bit 130", [] {
    static constexpr auto max = tls::polyMask;
    then("the modulus is reduced to zero and its predecessor stays canonical", [&] {
        tls::PolyLimbs predecessor{max - 5, max, max, max, max};
        const auto     unchanged = predecessor;
        tls::reducePoly(predecessor);
        require(predecessor == unchanged);
        tls::PolyLimbs prime{max - 4, max, max, max, max};
        tls::reducePoly(prime);
        require(prime == tls::PolyLimbs{});
    });
    then("a carry crosses the five limbs and folds back as five", [&] {
        tls::PolyLimbs wide{max + 1, max, max, max, max};
        tls::reducePoly(wide);
        require(wide == (tls::PolyLimbs{5, 0, 0, 0, 0}));
    });
});

const Scenario tlsCredentials("TLS loads matching PEM formats and preserves the published identity on reload failure", [] {
    const auto                 fixtures    = std::filesystem::path(WEBFRONT_SOURCE_DIR) / "test/web/fixtures/tls";
    const auto                 certificate = fixtures / "cert.pem";
    const auto                 sec1        = fixtures / "key-sec1.pem";
    webfront::tls::Credentials credentials(certificate, fixtures / "key.pem");
    const auto                 pkcs8 = credentials.snapshot();
    credentials.reload(certificate, sec1);
    const auto expected = credentials.snapshot();
    then("the two key encodings produce the same key", [&] {
        require(expected != pkcs8);
        require(tls::equalSecret(expected->key, pkcs8->key));
    });

    webfront::test::TemporaryDirectory temporary;
    const auto                         invalid  = temporary.path() / "invalid.pem";
    const auto                         writePem = [](const auto& path, std::string_view label, Bytes der) {
        std::ofstream(path, std::ios::app) << "-----BEGIN " << label << "-----\n" << webfront::base64::encode(der) << "\n-----END " << label << "-----\n";
    };
    then("an invalid private key PEM preserves the identity", [&] {
        std::ofstream(invalid) << "-----BEGIN EC PRIVATE KEY-----\nAAAA????\n-----END EC PRIVATE KEY-----\n";
        requireThrows<tls::Error>([&] {
            credentials.reload(certificate, invalid);
        });
        require(credentials.snapshot() == expected);
    });
    then("a nonmatching private key preserves the identity", [&] {
        std::string source;
        tls::readPemFile(sec1, source);
        std::string_view pem = source;
        auto             der = tls::pem(pem, "EC PRIVATE KEY");
        tls::Reader      outer{der};
        tls::Reader      key{outer.der(0x30)};
        key.der(2);
        const auto scalar                                          = key.der(4);
        der[static_cast<std::size_t>(scalar.data() - der.data())] ^= 1;
        const auto keyPath                                         = temporary.path() / "other-key.pem";
        writePem(keyPath, "EC PRIVATE KEY", std::move(der));
        requireThrows<tls::Error>(
            [&] {
                credentials.reload(certificate, keyPath);
            },
            contains("certificate and private key do not match"));
        require(credentials.snapshot() == expected);
    });
    then("an unsigned leaf or malformed intermediate preserves the identity", [&] {
        std::string source;
        tls::readPemFile(certificate, source);
        std::string_view pem   = source;
        auto             leaf  = tls::pem(pem, "CERTIFICATE");
        const auto       chain = temporary.path() / "chain.pem";
        writePem(chain, "CERTIFICATE", leaf);
        writePem(chain, "CERTIFICATE", Bytes{0});
        requireThrows<tls::Error>([&] {
            credentials.reload(chain, sec1);
        });
        require(credentials.snapshot() == expected);

        require(leaf[1] == 0x82);  // The fixture uses a two-byte DER length.
        tls::Reader input{leaf};
        tls::Reader outer{input.der(0x30)};
        outer.der(0x30);
        const auto size = leaf.size() - outer.remaining.size();
        leaf.resize(size);
        leaf[2]                 = static_cast<tls::Byte>((size - 4) >> 8);
        leaf[3]                 = static_cast<tls::Byte>(size - 4);
        const auto unsignedLeaf = temporary.path() / "unsigned.pem";
        writePem(unsignedLeaf, "CERTIFICATE", std::move(leaf));
        requireThrows<tls::Error>([&] {
            credentials.reload(unsignedLeaf, sec1);
        });
        require(credentials.snapshot() == expected);
    });
});
}  // namespace
