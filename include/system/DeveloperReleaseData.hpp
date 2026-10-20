#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-L2Viij7n7EXQZhsdiqAMHfUp7n/BVrPE8pt27kADOIuukPwvf3iiXFXevDQjToEOaDsZEWUCzxrA0WV9CvdDkQ=="};
} // namespace webfront::developer::release
