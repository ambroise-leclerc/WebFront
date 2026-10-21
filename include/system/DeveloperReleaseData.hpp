#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-Vm9fTWid4ecHVFYIPIer7LtsIZ0Hqk1gjJ9k5rNp8Nig0uS/C6u7FQIiJqXmAp1H6v0cMun3kd1eyUuvdb2Z/w=="};
} // namespace webfront::developer::release
