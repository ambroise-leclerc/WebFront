#pragma once
#include "detail/Wire.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace webfront::tls {
namespace detail {
inline constexpr std::size_t         maxCredentialBytes = 256 * 1024;
inline constexpr std::array<Byte, 7> ecPublicKey{0x2a, 0x86, 0x48, 0xce, 0x3d, 2, 1};
inline constexpr std::array<Byte, 8> prime256v1{0x2a, 0x86, 0x48, 0xce, 0x3d, 3, 1, 7};

inline void ecAlgorithm(Reader oid) {
    // AlgorithmIdentifier carries the algorithm OID followed by the named-curve OID.
    if (!std::ranges::equal(oid.der(6), ecPublicKey) || !std::ranges::equal(oid.der(6), prime256v1))
        throw Error("expected P-256 algorithm");
    oid.end();
}

inline View certificateKey(View certificate) {
    Reader input{certificate};
    Reader outer{input.der(0x30)};
    input.end();
    Reader body{outer.der(0x30)};
    if (!body.empty() && body.remaining.front() == 0xa0)
        body.der(0xa0);
    for (const Byte type : std::array<Byte, 5>{2, 0x30, 0x30, 0x30, 0x30})
        body.der(type);  // serial, signature, issuer, validity, subject
    Reader publicKey{body.der(0x30)};
    ecAlgorithm({publicKey.der(0x30)});
    const auto point = publicKey.der(3);
    publicKey.end();
    if (point.size() != 66 || point[0] != 0 || point[1] != 4)
        throw Error("expected uncompressed certificate key");
    return point.subspan(2);
}

inline Secret<> privateKey(View input, bool pkcs8) {
    Reader source{input};
    Reader key{source.der(0x30)};
    source.end();
    auto keyVersion = key.der(2);
    if (pkcs8) {
        if (keyVersion.size() != 1 || keyVersion.front() != 0)
            throw Error("expected PKCS#8 private key");
        ecAlgorithm({key.der(0x30)});
        Reader wrapped{key.der(4)};
        key = {wrapped.der(0x30)};
        wrapped.end();
        keyVersion = key.der(2);
    }
    if (keyVersion.size() != 1 || keyVersion.front() != 1)
        throw Error("expected EC private key");
    const auto scalar = key.der(4);
    if (scalar.size() != 32)
        throw Error("invalid EC private key size");
    if (!key.empty() && key.remaining.front() == 0xa0) {
        Reader parameters{key.der(0xa0)};
        if (!std::ranges::equal(parameters.der(6), prime256v1))
            throw Error("expected P-256 private key");
        parameters.end();
    } else if (!pkcs8) {
        throw Error("EC private key must name its curve");
    }
    if (!key.empty())
        key.der(0xa1);
    key.end();
    Secret<> result;
    std::ranges::copy(scalar, result.begin());
    return result;
}

inline Bytes pem(std::string_view& source, std::string_view label) {
    constexpr std::string_view whitespace = " \r\n\t";
    const auto                 first      = source.find_first_not_of(whitespace);
    if (first == source.npos)
        return {};
    source.remove_prefix(first);
    const auto begin = "-----BEGIN " + std::string(label) + "-----";
    const auto end   = "-----END " + std::string(label) + "-----";
    if (!source.starts_with(begin))
        throw Error("unexpected PEM block");
    source.remove_prefix(begin.size());
    const auto stop = source.find(end);
    if (stop == source.npos)
        throw Error("unterminated PEM block");
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes                      decoded;
    decoded.reserve(stop / 4 * 3 + 3);
    unsigned bits = 0, count = 0, padding = 0;
    for (const auto character : source.substr(0, stop)) {
        if (whitespace.find(character) != whitespace.npos)
            continue;
        if (character == '=') {
            ++padding;
            continue;
        }
        const auto digit = alphabet.find(character);
        if (digit == alphabet.npos || padding)
            throw Error("invalid PEM base64");
        bits   = (bits << 6) | static_cast<unsigned>(digit);
        count += 6;
        if (count >= 8) {
            count -= 8;
            decoded.push_back(static_cast<Byte>(bits >> count));
        }
    }
    if (decoded.empty() || count != 2 * padding || padding > 2 || (bits & ((1u << count) - 1)) != 0)
        throw Error("invalid PEM padding");
    source.remove_prefix(stop + end.size());
    return decoded;
}

inline void readPemFile(const std::filesystem::path& path, std::string& contents) {
    std::ifstream file(path, std::ios::binary);
    contents.resize(maxCredentialBytes + 1);
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file.eof() || file.bad())
        throw Error("cannot read bounded PEM file");
    contents.resize(static_cast<std::size_t>(file.gcount()));
}

struct Identity {
    Bytes    certificateMessage;
    Secret<> key;
};

inline std::shared_ptr<const Identity> loadIdentity(const std::filesystem::path& chainPath, const std::filesystem::path& keyPath) {
    std::string chain, key;
    Bytes       der;
    struct Wipe {
        std::string& key;
        Bytes&       der;
        ~Wipe() {
            wipe(key.data(), key.size());
            wipe(der.data(), der.size());
        }
    } erase{key, der};
    readPemFile(chainPath, chain);
    readPemFile(keyPath, key);
    std::string_view chainPem = chain, keyPem = key;
    const auto       keyStart = keyPem.find_first_not_of(" \r\n\t");
    const bool       pkcs8    = keyStart != keyPem.npos && keyPem.substr(keyStart).starts_with("-----BEGIN PRIVATE KEY-----");
    der                       = pem(keyPem, pkcs8 ? "PRIVATE KEY" : "EC PRIVATE KEY");
    if (keyPem.find_first_not_of(" \r\n\t") != keyPem.npos)
        throw Error("extra PEM key material");
    auto identity = std::make_shared<Identity>();
    identity->key = privateKey(der, pkcs8);
    auto scalar   = decode(identity->key);
    if (!P256Order::validScalar(scalar)) {
        wipe(scalar.data(), sizeof(scalar));
        throw Error("invalid private key scalar");
    }
    const auto [x, y] = p256Base(scalar);
    wipe(scalar.data(), sizeof(scalar));
    std::array<Byte, 64> point;
    std::ranges::copy(encode(x), point.begin());
    std::ranges::copy(encode(y), point.begin() + 32);
    auto leaf = pem(chainPem, "CERTIFICATE");
    if (!std::ranges::equal(certificateKey(leaf), point))
        throw Error("certificate and private key do not match");

    Bytes entries;
    for (auto certificate = std::move(leaf); !certificate.empty(); certificate = pem(chainPem, "CERTIFICATE")) {
        vector(entries, certificate, 3);
        put(entries, 0, 2);  // CertificateEntry extensions
    }
    Bytes body{0};           // empty certificate_request_context
    vector(body, entries, 3);
    identity->certificateMessage = handshake(11, body);
    return identity;
}
}  // namespace detail

/// The PEM chain starts with the P-256 leaf. Reload publishes both files atomically or throws.
class Credentials {
public:
    Credentials(const std::filesystem::path& chain, const std::filesystem::path& key) : identity(detail::loadIdentity(chain, key)) {}

    void reload(const std::filesystem::path& chain, const std::filesystem::path& key) {
        auto             next = detail::loadIdentity(chain, key);
        std::scoped_lock lock(mutex);
        identity.swap(next);
    }

    [[nodiscard]] auto snapshot() const {
        std::scoped_lock lock(mutex);
        return identity;
    }

private:
    mutable std::mutex                      mutex;
    std::shared_ptr<const detail::Identity> identity;
};
}  // namespace webfront::tls
