#pragma once
#include "Wire.hpp"

#include <optional>
#include <system_error>
#include <utility>

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
    bool     earlyData  = false;
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
    bool                      supportsTls13 = false, signature = false, supportedGroup = false;
    std::array<unsigned, 128> seen;
    std::size_t               extensionCount    = 0;
    const auto                containsExtension = [&](unsigned kind) {
        const auto prior = std::span(seen).first(extensionCount);
        return std::ranges::find(prior, kind) != prior.end();
    };
    View shares, groups;
    while (!extensions.empty()) {
        const auto encoded = extensions.remaining;
        const auto kind    = extensions.number(2);
        Reader     extension{extensions.vector(2)};
        if (containsExtension(kind))
            throw Error("duplicate ClientHello extension", 47);
        if (extensionCount == seen.size())
            throw Error("too many ClientHello extensions");
        seen[extensionCount++] = kind;
        switch (kind) {
            case 43:
                supportsTls13 = contains(extension.vector(1), 2, 0x0304);
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
                result.earlyData = true;
                break;
            case 45:
                if (extension.vector(1).empty())
                    throw Error("empty psk_key_exchange_modes", 50);
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
        if (kind == 43 || kind == 13 || kind == 10 || kind == 51 || kind == 42 || kind == 45 || kind == 16)
            extension.end();
        if (kind != 51 && kind != 21 && kind != 41 && kind != 42)
            invariant.update(encoded.first(encoded.size() - extensions.remaining.size()));
    }
    if (!supportsTls13)
        throw Error("TLS 1.3 required", 70);
    if (!containsExtension(13))
        throw Error("missing signature_algorithms", 109);
    if (!containsExtension(51))
        throw Error("missing key_share", 109);
    if (!offeredSuite || !signature || !supportedGroup)
        throw Error("no common cipher suite, signature or group", 40);
    const bool hasPsk = containsExtension(41);
    if (hasPsk && !containsExtension(45))
        throw Error("PSK offer missing psk_key_exchange_modes", 109);
    if (result.earlyData && (retry || !hasPsk))
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
    constexpr std::array<Byte, 2> tls13{3, 4};
    Bytes                         extensions;
    extension(extensions, 43, tls13);
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

inline RecordHeader parseRecord(View header, bool encrypted) {
    Reader     input{header};
    const auto type          = static_cast<Byte>(input.number(1));
    const auto recordVersion = input.number(2);
    const auto size          = input.number(2);
    input.end();
    // TLSPlaintext's legacy_record_version is ignored (RFC 8446, section 5.1); only protected records carry the fixed value.
    if (type == applicationData && recordVersion != 0x0303)
        throw Error("invalid record version", 70);
    if (size > (encrypted && type == applicationData ? maxCiphertext : maxPlaintext))
        throw Error("oversized record", 22);
    if (size == 0 || (encrypted && type == applicationData && size < 17))
        throw Error("invalid record length", 50);
    return {type, size};
}

inline Nonce recordNonce(Nonce nonce, std::uint64_t sequence) {
    if (sequence == UINT64_MAX)
        throw std::system_error(std::make_error_code(std::errc::value_too_large), "TLS record sequence exhausted");
    for (unsigned i = 0; i != 8; ++i)
        nonce[11 - i] ^= static_cast<Byte>(sequence >> (8 * i));
    return nonce;
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
        const auto nonce      = recordNonce(iv, sequence);
        ++sequence;
        auto payload = std::span(record).subspan(5, size + 1);
        chachaXor(key, nonce, payload);
        std::ranges::copy(polyTag(key, nonce, View(record).first(5), payload), record.data() + 6 + size);
    }

    Byte open(View header, Bytes& payload) {
        if (payload.size() < 17)
            throw Error("short ciphertext");
        const auto nonce       = recordNonce(iv, sequence);
        const auto size        = payload.size() - 16;
        const auto ciphertext  = std::span(payload).first(size);
        const auto expectedTag = polyTag(key, nonce, header, ciphertext);
        if (!equalSecret(expectedTag, View(payload).subspan(size)))
            throw Error("invalid authentication tag", 20);
        ++sequence;
        chachaXor(key, nonce, ciphertext);
        payload.resize(size);
        while (!payload.empty() && payload.back() == 0)
            payload.pop_back();
        if (payload.empty())
            throw Error("missing inner record type", 10);
        const auto type = payload.back();
        payload.pop_back();
        // RFC 8446, section 5.4: padding follows the type, so the content limit applies once it is removed. The ciphertext
        // bound (maxCiphertext) was checked by parseRecord.
        if (payload.size() > maxPlaintext)
            throw Error("oversized inner plaintext", 22);
        return type;
    }

private:
    Secret<>             key{};
    std::array<Byte, 12> iv{};
    std::uint64_t        sequence = 0;
};
// Serialized by the socket's context. A close cannot overtake an in-flight write.
struct RecordControl {
    bool sending         = false;
    bool closed          = false;
    bool updateRequested = false;
    bool peerNotified    = false;

    bool beginClose(Byte failure = 0) {
        if (std::exchange(closed, true))
            return false;
        pendingAlert = failure;
        return !sending;
    }

    void transportLost() {
        closed = true;
        pendingAlert.reset();
    }

    std::optional<Byte> takeAlert() {
        return std::exchange(pendingAlert, std::nullopt);
    }

    // Returns true for a nonempty application record ready to deliver.
    bool receive(Byte type, View input, Traffic& traffic, Secret<>& secret) {
        if (type == alert && input.size() == 2) {
            // TLS 1.3 uses the description; user_canceled (90) can precede close_notify.
            if (input[1] == 90) {
                if (updateSize != 0)
                    throw Error("alert interrupted a KeyUpdate", 10);
                if (++skipped > 32)
                    throw Error("too many consecutive control or empty records", 10);
                return false;
            }
            if (input[1] != 0) {
                transportLost();
                throw std::system_error(std::make_error_code(std::errc::connection_aborted));
            }
            if (updateSize != 0)
                throw Error("close_notify interrupted a KeyUpdate", 10);
            peerNotified = true;
            return false;
        }
        if (type == handshakeRecord) {
            constexpr std::array<Byte, 4> header{24, 0, 0, 1};
            if (input.empty() || input.size() > update.size() - updateSize)
                throw Error("invalid KeyUpdate size", 10);
            std::ranges::copy(input, update.data() + updateSize);
            updateSize += input.size();
            if (!std::ranges::equal(View(update).first(std::min(updateSize, header.size())), View(header).first(std::min(updateSize, header.size()))))
                throw Error("unexpected post-handshake message", 10);
            if (updateSize == update.size()) {
                if (update.back() > 1)
                    throw Error("invalid KeyUpdate request", 47);
                secret = {expandLabel(secret, "traffic upd")};
                traffic.install(secret);
                updateRequested |= update.back() == 1;
                updateSize       = 0;
            }
        } else if (type == applicationData && updateSize == 0) {
            if (!input.empty()) {
                skipped = 0;
                return true;
            }
        } else {
            throw Error("unexpected application record", 10);
        }
        if (++skipped > 32)
            throw Error("too many consecutive control or empty records", 10);
        return false;
    }

private:
    std::optional<Byte> pendingAlert;
    std::array<Byte, 5> update{};
    std::size_t         updateSize = 0;
    unsigned            skipped    = 0;
};
}  // namespace webfront::tls::detail
