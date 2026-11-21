#pragma once
#include "Crypto.hpp"

namespace webfront::tls::detail {
struct Error : std::runtime_error {
    Error(std::string_view message, Byte code = 50) : std::runtime_error("TLS: " + std::string(message)), alert(code) {}
    Byte alert;
};

struct Reader {
    View remaining;

    View take(std::size_t length) {
        if (length > remaining.size())
            throw Error("truncated message");
        const auto value = remaining.first(length);
        remaining        = remaining.subspan(length);
        return value;
    }

    unsigned number(unsigned width) {
        unsigned value = 0;
        for (auto byte : take(width))
            value = (value << 8) | byte;
        return value;
    }

    View vector(unsigned width) {
        return take(number(width));
    }
    bool empty() const {
        return remaining.empty();
    }
    void end() const {
        if (!empty())
            throw Error("trailing bytes");
    }

    View der(Byte type) {
        if (number(1) != type)
            throw Error("unexpected DER tag");
        auto size = number(1);
        if (size & 128) {
            const auto width = size & 127;
            if (width == 0 || width > 3)
                throw Error("invalid DER length");
            size = number(width);
            if (size < (width == 1 ? 128u : 1u << (8 * (width - 1))))
                throw Error("noncanonical DER length");
        }
        return take(size);
    }
};

inline void vector(Bytes& out, View data, unsigned width) {
    put(out, data.size(), width);
    append(out, data);
}

inline Bytes handshake(Byte type, View body) {
    Bytes message{type};
    vector(message, body, 3);
    return message;
}

inline void extension(Bytes& out, unsigned type, View body) {
    put(out, type, 2);
    vector(out, body, 2);
}
}  // namespace webfront::tls::detail
