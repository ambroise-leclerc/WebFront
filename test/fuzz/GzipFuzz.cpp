#include <webfront/http/Gzip.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Bound expansion even for valid compression bombs. Invalid input is an expected result.
    (void)webfront::http::gzip::decodeGzip(std::string_view(reinterpret_cast<const char*>(data), size), 1024 * 1024);
    return 0;
}
