#include "GzipZlib.hpp"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    constexpr std::size_t  limit  = 1024 * 1024;
    const auto             actual = webfront::http::gzip::decodeGzip(bytes, limit);
    // Arbitrary zlib-accepted inputs may use unsupported reserved distance declarations.
    // Generated supported streams are checked in both directions by gzip_differential.
    if (actual && actual != gziptest::reference(bytes, limit))
        std::abort();
    return 0;
}
