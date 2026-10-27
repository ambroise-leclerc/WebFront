/// @brief Bounded DOM mutation wire command (ADR-003); the document API is a separate layer.
#pragma once

#include "Messages.hpp"

#include <utility>

namespace webfront::msg {

enum class DomOp : std::uint8_t {
    name = 1,
    element,
    text,
    attribute,
    unsetAttribute,
    property,
    setText,
    append,
    insertBefore,
    remove,
    listen,
    unlisten,
    mount
};

/// Operands follow the order in ADR-003. c is the flags byte for listen, or the reference for insertBefore.
struct DomOperation {
    DomOp         code;
    std::uint32_t a{}, b{}, c{};
    std::string   value{};
    bool          operator==(const DomOperation&) const = default;
};

/** @brief An owning command; all integers are canonical uint32 LEB128 except the fixed little-endian batch id. */
class DomBatch {
public:
    static constexpr std::size_t maxBytes      = 1024 * 1024;  // Complete command, including the six-byte header.
    static constexpr std::size_t headerBytes   = 6;
    static constexpr std::size_t maxOperations = 131072;

    explicit DomBatch(std::uint32_t id) : bytes{std::byte(Command::domBatch), std::byte{1}} {
        if (id == 0)
            throw std::invalid_argument("DOM batch id zero is reserved");
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(std::byte((id >> shift) & 255));
    }

    /// Appends atomically: an invalid UTF-8 string or an oversized operation leaves this batch unchanged.
    void add(const DomOperation& op) {
        if (operationCount == maxOperations)
            throw std::length_error("DOM batch operation limit exceeded");
        const auto [integers, string] = layout(op.code);
        const std::array values{op.a, op.b, op.c};
        for (unsigned i = integers; i < values.size(); ++i)
            if (values[i] != 0 && !(op.code == DomOp::listen && i == 2))
                throw std::invalid_argument("Nonzero unused DOM operand");
        if (!string && !op.value.empty())
            throw std::invalid_argument("Unused DOM string operand");
        std::vector<std::byte> encoded{std::byte(op.code)};
        for (unsigned i = 0; i < integers; ++i)
            putVarint(encoded, values[i]);
        if (op.code == DomOp::listen) {
            if (op.c > 3)
                throw std::invalid_argument("Invalid DOM listener flags");
            encoded.push_back(std::byte(op.c));
        }
        if (string) {
            if (op.value.size() > maxBytes)
                throw std::length_error("DOM batch exceeds 1 MiB");
            validateUtf8(op.value);
            putVarint(encoded, static_cast<std::uint32_t>(op.value.size()));
            const auto raw = std::as_bytes(std::span(op.value));
            encoded.insert(encoded.end(), raw.begin(), raw.end());
        }
        if (encoded.size() > maxBytes - bytes.size())
            throw std::length_error("DOM batch exceeds 1 MiB");
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        ++operationCount;
    }

    [[nodiscard]] std::span<const std::byte> header() const {
        return std::span(bytes).first(headerBytes);
    }
    [[nodiscard]] std::span<const std::byte> payload() const {
        return std::span(bytes).subspan(headerBytes);
    }
    [[nodiscard]] std::span<const std::byte> data() const {
        return bytes;
    }

    /// Structural decoder; node ownership, names and the allowlist are validated by the browser interpreter.
    static std::pair<std::uint32_t, std::vector<DomOperation>> decode(std::span<const std::byte> raw) {
        if (raw.size() > maxBytes)
            throw std::length_error("DOM batch exceeds 1 MiB");
        if (raw.size() < headerBytes || raw[0] != std::byte(Command::domBatch) || raw[1] != std::byte{1})
            throw std::invalid_argument("Invalid DOM batch header");
        std::uint32_t id{};
        for (unsigned i = 0; i < 4; ++i)
            id |= std::uint32_t(raw[2 + i]) << (8 * i);
        if (id == 0)
            throw std::invalid_argument("DOM batch id zero is reserved");
        Reader      counter{raw.subspan(headerBytes)};
        std::size_t count{};
        while (!counter.raw.empty()) {
            if (count == maxOperations)
                throw std::length_error("DOM batch operation limit exceeded");
            readOperation(counter, false);
            ++count;
        }
        Reader                    input{raw.subspan(headerBytes)};
        std::vector<DomOperation> operations;
        operations.reserve(count);
        while (!input.raw.empty())
            operations.push_back(readOperation(input, true));
        return {id, std::move(operations)};
    }

private:
    std::vector<std::byte> bytes;
    std::size_t            operationCount{};

    static std::pair<unsigned, bool> layout(DomOp code) {
        switch (code) {
            case DomOp::name:
            case DomOp::text:
            case DomOp::setText:
                return {1, true};
            case DomOp::attribute:
            case DomOp::property:
                return {2, true};
            case DomOp::element:
            case DomOp::unsetAttribute:
            case DomOp::append:
            case DomOp::listen:
            case DomOp::unlisten:
            case DomOp::mount:
                return {2, false};
            case DomOp::insertBefore:
                return {3, false};
            case DomOp::remove:
                return {1, false};
        }
        throw std::invalid_argument("Unknown DOM operation");
    }

    static void putVarint(std::vector<std::byte>& out, std::uint32_t value) {
        do {
            const auto low = value & 127;
            value        >>= 7;
            out.push_back(std::byte(low | (value ? 128 : 0)));
        } while (value);
    }

    struct Reader {
        std::span<const std::byte> raw;
        std::uint8_t               byte() {
            if (raw.empty())
                throw std::invalid_argument("Truncated DOM batch");
            const auto value = std::uint8_t(raw.front());
            raw              = raw.subspan(1);
            return value;
        }
        std::uint32_t varint() {
            std::uint32_t value{};
            for (unsigned i = 0; i < 5; ++i) {
                const auto next = byte();
                if ((i == 4 && next > 15) || (i && next == 0))
                    throw std::invalid_argument("Noncanonical or overflowing DOM varint");
                value |= std::uint32_t(next & 127) << (7 * i);
                if (!(next & 128))
                    return value;
            }
            throw std::invalid_argument("Overflowing DOM varint");
        }
    };

    // First pass validates structure/count without allocating strings; the owning pass checks UTF-8 before copying.
    static DomOperation readOperation(Reader& input, bool ownStrings) {
        DomOperation op{static_cast<DomOp>(input.byte())};
        const auto [integers, string] = layout(op.code);
        std::array<std::uint32_t*, 3> values{&op.a, &op.b, &op.c};
        for (unsigned i = 0; i < integers; ++i)
            *values[i] = input.varint();
        if (op.code == DomOp::listen) {
            op.c = input.byte();
            if (op.c > 3)
                throw std::invalid_argument("Invalid DOM listener flags");
        }
        if (string) {
            const auto size = input.varint();
            if (size > input.raw.size())
                throw std::invalid_argument("Truncated DOM string");
            if (ownStrings) {
                const std::string_view value(reinterpret_cast<const char*>(input.raw.data()), size);
                validateUtf8(value);
                op.value.assign(value);
            }
            input.raw = input.raw.subspan(size);
        }
        return op;
    }

    static void validateUtf8(std::string_view value) {
        std::size_t i = 0;
        while (i < value.size()) {
            const auto lead = static_cast<unsigned char>(value[i++]);
            if (lead < 128)
                continue;
            unsigned      count;
            std::uint32_t code;
            if (lead >= 0xc2 && lead <= 0xdf) {
                count = 1;
                code  = lead & 31;
            } else if (lead >= 0xe0 && lead <= 0xef) {
                count = 2;
                code  = lead & 15;
            } else if (lead >= 0xf0 && lead <= 0xf4) {
                count = 3;
                code  = lead & 7;
            } else
                throw std::invalid_argument("Invalid DOM UTF-8");
            const auto minimum = count == 1 ? 128u : count == 2 ? 2048u : 65536u;
            if (count > value.size() - i)
                throw std::invalid_argument("Truncated DOM UTF-8");
            while (count--) {
                const auto next = static_cast<unsigned char>(value[i++]);
                if ((next & 0xc0) != 0x80)
                    throw std::invalid_argument("Invalid DOM UTF-8 continuation");
                code = (code << 6) | (next & 63);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                throw std::invalid_argument("Invalid DOM UTF-8 code point");
        }
    }
};

}  // namespace webfront::msg
