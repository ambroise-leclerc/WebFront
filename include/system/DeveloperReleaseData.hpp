#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-S0MOT4vibRRKNP5D8DoI0NTV5Yw8diiGsy+B62XoDVvqLfb/gH7eBQF5cvnkbIZ5UW+2xuzmI2e/KP6oyqH9AQ=="};
} // namespace webfront::developer::release
