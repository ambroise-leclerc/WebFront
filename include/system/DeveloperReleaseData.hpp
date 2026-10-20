#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-8L+xaG7dSuMKDa+/7fInSKCnGeePVxySDUNMChsSDJq11y+tyr7lJMTwFTacS4Xx+uWRAer2MZ+8+KKBO1KT3A=="};
} // namespace webfront::developer::release
