#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.1"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-SQF7wFYWpcBcu/JT27vo7XFan1PsXtDHWr01IaqTCBYuCKbWfxdbTstW9FUYu6khWDpvnTXDNKrIZvQl74tRng=="};
} // namespace webfront::developer::release
