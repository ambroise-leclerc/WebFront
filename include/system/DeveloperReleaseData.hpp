#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-dXjGKIxPze87x2c8UiyJ0ij+ekFX5g0Cn2jgtVnQQp6qhE7kw6rkBE2nLvinW12gPqPUBBpKdXvBpp5jpBrA/A=="};
} // namespace webfront::developer::release
