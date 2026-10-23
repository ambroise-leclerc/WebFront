#pragma once

// Pinned developer distribution. Regenerate with js/developer/tools/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.6.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-xidz8ggQFo/UUTttIii+3FRJmGpQPoHhbmk9Fv/wxRA4EKEhD5WeiAcstsuY8H/wXc/jo/bljIYkzyyyrJ1oEg=="};
} // namespace webfront::developer::release
