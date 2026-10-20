#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.2.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.2.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-3Aeic8bF6+eoRAcuD3JZIQF6Oanexou/37yblbZe9U9bGBwXEyvuBQ+0BkdvhH+gof3+8gyi5koClBooAj/8/Q=="};
} // namespace webfront::developer::release
