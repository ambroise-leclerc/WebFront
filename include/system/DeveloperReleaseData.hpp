#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-CLB9//CA5dgCYOHOS/fpHNPm9vxOZ4neoFZiiU2qHNImQqlnyJmn1xlTp/rBwMzZN2l1/JqXxKt0IhD8OwBScg=="};
} // namespace webfront::developer::release
