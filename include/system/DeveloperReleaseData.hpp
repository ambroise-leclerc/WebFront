#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-C2vhOwbt9ICzV3cEwC3Uy6CJxsIEPhXHqO6X0vBF4Lh8jtxYEr8nF2DqKJJDA5ugwhTXa/i8JMkiU+iz1tS4kQ=="};
} // namespace webfront::developer::release
