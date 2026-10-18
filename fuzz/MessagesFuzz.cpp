#include <weblink/Messages.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
template <typename T>
void decode(std::span<const std::byte> bytes) {
    T value{};
    try {
        webfront::msg::FunctionCall<>::decodeParameter(value, bytes);
    } catch (const std::runtime_error&) {
        // Truncation, unsupported types and mismatches are expected parser results.
    }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span(data, size));
    using namespace webfront::msg;
    try {
        if (!bytes.empty()) {
            switch (static_cast<Command>(bytes.front())) {
                case Command::handshake:
                    (void)Handshake::castFromRawData(bytes)->getEndian();
                    break;
                case Command::callFunction:
                    (void)FunctionCall<>::castFromRawData(bytes)->getFunctionName();
                    break;
                case Command::functionReturn:
                    (void)FunctionReturn<>::castFromRawData(bytes)->payload();
                    break;
                default:
                    break;
            }
        }
    } catch (const std::runtime_error&) { }
    // Try every owning result family, including mismatches; no recursive untrusted types.
    decode<bool>(bytes);
    decode<std::int8_t>(bytes);
    decode<std::uint8_t>(bytes);
    decode<std::int16_t>(bytes);
    decode<std::uint16_t>(bytes);
    decode<std::int32_t>(bytes);
    decode<std::uint32_t>(bytes);
    decode<std::int64_t>(bytes);
    decode<std::uint64_t>(bytes);
    decode<float>(bytes);
    decode<double>(bytes);
    decode<std::string>(bytes);
    decode<std::tuple<bool, std::string, double>>(bytes);
    decode<std::vector<std::uint8_t>>(bytes);
    decode<std::vector<std::int8_t>>(bytes);
    decode<std::vector<std::uint16_t>>(bytes);
    decode<std::vector<std::int16_t>>(bytes);
    decode<std::vector<std::uint32_t>>(bytes);
    decode<std::vector<std::int32_t>>(bytes);
    decode<std::vector<std::uint64_t>>(bytes);
    decode<std::vector<std::int64_t>>(bytes);
    decode<std::vector<float>>(bytes);
    decode<std::vector<double>>(bytes);
    decode<std::array<std::uint8_t, 3>>(bytes);
    return 0;
}
