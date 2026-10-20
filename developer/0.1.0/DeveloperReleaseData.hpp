#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.1.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.1.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-diiDXu7W4X0sa8jJMJe+lwKUh5zpeaALdODcx3+VxM19upHPB2fgOtxndm0cynJYk6oa6sTpRrUActBq/u7AcQ=="};
} // namespace webfront::developer::release
