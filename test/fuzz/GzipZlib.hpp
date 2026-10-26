#pragma once

#include <webfront/http/Gzip.hpp>

#include <array>
#include <limits>
#include <stdexcept>

#include <zlib.h>

namespace gziptest {
// Test-only zlib oracle. Require complete consumption, concatenation and the same total budget.
inline std::optional<std::string> reference(std::string_view bytes, std::size_t limit) {
    if (bytes.size() > std::numeric_limits<uInt>::max())
        throw std::runtime_error("zlib oracle input exceeds uInt");
    z_stream stream{};
    if (inflateInit2(&stream, 31) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");
    struct Cleanup {
        z_stream& stream;
        ~Cleanup() {
            inflateEnd(&stream);
        }
    } cleanup{stream};
    stream.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
    stream.avail_in = static_cast<uInt>(bytes.size());
    std::string             output;
    std::array<char, 16384> block{};
    for (;;) {
        stream.next_out     = reinterpret_cast<Bytef*>(block.data());
        stream.avail_out    = block.size();
        const auto before   = stream.avail_in;
        const auto status   = inflate(&stream, Z_NO_FLUSH);
        const auto produced = block.size() - stream.avail_out;
        if (produced > limit - output.size())
            return std::nullopt;
        output.append(block.data(), produced);
        if (status == Z_STREAM_END) {
            if (stream.avail_in == 0)
                return output;
            if (inflateReset(&stream) != Z_OK)
                throw std::runtime_error("inflateReset failed");
        } else if (status != Z_OK || (before == stream.avail_in && produced == 0)) {
            return std::nullopt;
        }
    }
}

inline void compare(std::string_view bytes, std::size_t limit) {
    if (webfront::http::gzip::decodeGzip(bytes, limit) != reference(bytes, limit))
        throw std::runtime_error("gzip/zlib acceptance or content mismatch");
}
}  // namespace gziptest
