#pragma once
#include "Wire.hpp"

#include <bitset>
#include <optional>

namespace webfront::tls::detail {
inline constexpr unsigned suite = 0x1303, group = 29, signatureScheme = 0x0403;
inline constexpr unsigned maxPlaintext = 16384, maxCiphertext = maxPlaintext + 256;
inline constexpr Digest   zeros{};
inline constexpr Digest   retryRandom{0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
                                    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};

enum Record : Byte { changeCipherSpec = 20, alert = 21, handshakeRecord = 22, applicationData = 23 };

inline bool contains(View list, unsigned width, unsigned value) {
    Reader elements{list};
    if (list.empty() || list.size() % width != 0)
        throw Error("invalid negotiation list");
    while (!elements.empty())
        if (elements.number(width) == value)
            return true;
    return false;
}

struct ClientHello {
    View     session;
    View     share;
    Digest   invariant;
    unsigned shareCount = 0;
    bool     alpn       = false;
};

inline ClientHello parseHello(View message, bool retry) {
    Reader header{message};
    if (header.number(1) != 1)
        throw Error("expected ClientHello", 10);
    Reader hello{header.vector(3)};
    header.end();
    const auto original = hello.remaining;
    if (hello.number(2) != 0x0303)
        throw Error("invalid ClientHello legacy version", 70);
    hello.take(32);
    ClientHello result;
    result.session = hello.vector(1);
    if (result.session.size() > 32)
        throw Error("oversized session ID");
    const bool offeredSuite = contains(hello.vector(2), 2, suite);
    const auto compression  = hello.vector(1);
    if (compression.size() != 1 || compression.front() != 0)
        throw Error("invalid compression methods", 47);
    Sha256 invariant;
    invariant.update(original.first(original.size() - hello.remaining.size()));
    Reader extensions{hello.vector(2)};
    hello.end();
    bool               version = false, signature = false, supportedGroup = false, earlyData = false;
    std::bitset<65536> seen;
    View               shares, groups;
    while (!extensions.empty()) {
        const auto encoded = extensions.remaining;
        const auto kind    = extensions.number(2);
        Reader     extension{extensions.vector(2)};
        if (seen.test(kind))
            throw Error("duplicate ClientHello extension", 47);
        seen.set(kind);
        switch (kind) {
            case 43:
                version = contains(extension.vector(1), 2, 0x0304);
                break;
            case 13:
                signature = contains(extension.vector(2), 2, signatureScheme);
                break;
            case 10:
                groups         = extension.vector(2);
                supportedGroup = contains(groups, 2, group);
                break;
            case 51:
                shares = extension.vector(2);
                break;
            case 42:
                earlyData = true;
                break;
            case 44:
                throw Error("unexpected ClientHello cookie", 47);
            case 41:
                if (!extensions.empty())
                    throw Error("pre_shared_key is not last", 47);
                break;  // This server always negotiates a full handshake.
            case 16: {
                Reader protocols{extension.vector(2)};
                if (protocols.empty())
                    throw Error("empty ALPN list");
                while (!protocols.empty()) {
                    const auto protocol = protocols.vector(1);
                    if (protocol.empty())
                        throw Error("empty ALPN name");
                    result.alpn |= std::ranges::equal(protocol, bytes("http/1.1"));
                }
                if (!result.alpn)
                    throw Error("HTTP/1.1 not offered", 120);
                break;
            }
            default:
                break;
        }
        if (kind == 43 || kind == 13 || kind == 10 || kind == 51 || kind == 42 || kind == 16)
            extension.end();
        if (kind != 51 && kind != 21 && kind != 41 && kind != 42)
            invariant.update(encoded.first(encoded.size() - extensions.remaining.size()));
    }
    if (!version)
        throw Error("TLS 1.3 required", 70);
    if (!offeredSuite || !signature || !supportedGroup)
        throw Error("no common cipher suite, signature or group", 40);
    if (earlyData && (retry || !seen.test(41)))
        throw Error("unexpected early_data", 47);
    Reader      keys{shares};
    std::size_t groupOffset = 0;
    while (!keys.empty()) {
        const auto kind = keys.number(2);
        const auto key  = keys.vector(2);
        // Key shares are an ordered subsequence of supported_groups.
        while (groupOffset < groups.size() && Reader{groups.subspan(groupOffset)}.number(2) != kind)
            groupOffset += 2;
        if (groupOffset == groups.size() || key.empty())
            throw Error("invalid key share", 47);
        groupOffset += 2;
        ++result.shareCount;
        if (kind == group) {
            if (key.size() != 32 || !result.share.empty())
                throw Error("invalid X25519 key share", 47);
            result.share = key;
        }
    }
    result.invariant = invariant.digest();
    return result;
}

inline Bytes serverHello(View session, View random, View publicKey) {
    constexpr std::array<Byte, 2> version{3, 4};
    Bytes                         extensions;
    extension(extensions, 43, version);
    Bytes keyShare;
    put(keyShare, group, 2);
    if (!publicKey.empty())
        vector(keyShare, publicKey, 2);
    extension(extensions, 51, keyShare);

    Bytes body{3, 3};
    append(body, random);
    vector(body, session, 1);
    put(body, suite, 2);
    put(body, 0, 1);
    vector(body, extensions, 2);
    return handshake(2, body);
}

struct RecordHeader {
    Byte     type;
    unsigned size;
};

inline RecordHeader parseRecord(View header, bool encrypted, bool initial = false) {
    Reader     input{header};
    const auto type    = static_cast<Byte>(input.number(1));
    const auto version = input.number(2);
    const auto size    = input.number(2);
    input.end();
    if (version != 0x0303 && !(initial && version == 0x0301 && type == handshakeRecord))
        throw Error("invalid record version", 70);
    if (size > (encrypted && type == applicationData ? maxCiphertext : maxPlaintext))
        throw Error("oversized record", 22);
    if (size == 0 || (encrypted && type == applicationData && size < 17))
        throw Error("invalid record length", 50);
    return {type, size};
}

struct Traffic {
    void install(View secret) {
        key = {expandLabel(secret, "key")};
        const Secret<> expanded{expandLabel(secret, "iv", {}, 12)};
        std::ranges::copy(View(expanded).first(12), iv.begin());
        sequence = 0;
    }

    void seal(Byte type, Bytes& record) {
        const auto size = record.size() - 5;
        if (size > maxPlaintext)
            throw Error("oversized plaintext", 22);
        record.resize(record.size() + 17);
        record[0] = applicationData;
        record[1] = record[2] = 3;
        record[3]             = static_cast<Byte>((size + 17) >> 8);
        record[4]             = static_cast<Byte>(size + 17);
        record[5 + size]      = type;
        const auto nonce      = nextNonce();
        auto       payload    = std::span(record).subspan(5, size + 1);
        chachaXor(key, nonce, payload);
        std::ranges::copy(polyTag(key, nonce, View(record).first(5), payload), record.data() + 6 + size);
    }

    Byte open(View header, Bytes& payload) {
        if (payload.size() < 17)
            throw Error("short ciphertext");
        const auto nonce       = nextNonce();
        const auto size        = payload.size() - 16;
        const auto ciphertext  = std::span(payload).first(size);
        const auto expectedTag = polyTag(key, nonce, header, ciphertext);
        if (!equalSecret(expectedTag, View(payload).subspan(size)))
            throw Error("invalid authentication tag", 20);
        chachaXor(key, nonce, ciphertext);
        payload.resize(size);
        while (!payload.empty() && payload.back() == 0)
            payload.pop_back();
        if (payload.empty())
            throw Error("missing inner record type", 10);
        const auto type = payload.back();
        payload.pop_back();
        if (payload.size() > maxPlaintext)
            throw Error("oversized inner plaintext", 22);
        return type;
    }

private:
    Nonce nextNonce() {
        if (sequence == UINT64_MAX)
            throw Error("record sequence exhausted", 80);
        auto nonce = iv;
        for (unsigned i = 0; i != 8; ++i)
            nonce[11 - i] ^= static_cast<Byte>(sequence >> (8 * i));
        ++sequence;
        return nonce;
    }
    Secret<>             key{};
    std::array<Byte, 12> iv{};
    std::uint64_t        sequence = 0;
};
}  // namespace webfront::tls::detail
