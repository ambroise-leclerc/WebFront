#pragma once

#include "../http/Encodings.hpp"

#include <string>
#include <string_view>

namespace webfront::frontend::detail {

/// Host name in the URL every frontend opens. `localhost` may resolve to ::1 first, so a server bound
/// only to 127.0.0.1 relies on the browser falling back to IPv4.
inline constexpr std::string_view applicationHost = "localhost";

/// Builds the UTF-8 URL of an application page, shared by every frontend. `file` is a UTF-8 path:
/// '/' separates segments and stays literal, every other byte except RFC 3986 unreserved characters
/// is percent-encoded, so spaces and literal '#', '?' and '%' address the file instead of the URL syntax.
[[nodiscard]] inline std::string applicationUrl(std::string_view port, std::string_view file) {
    std::string url = "http://";
    url += applicationHost;
    url += ':';
    url += port;
    url += '/';
    for (std::size_t begin = 0;;) {
        const auto end = file.find('/', begin);
        url += uri::encode(file.substr(begin, end == std::string_view::npos ? end : end - begin));
        if (end == std::string_view::npos)
            break;
        url += '/';
        begin = end + 1;
    }
    return url;
}

}  // namespace webfront::frontend::detail
