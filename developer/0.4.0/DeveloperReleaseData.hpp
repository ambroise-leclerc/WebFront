#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.4.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.4.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-CElEaA68ASX+2Wsq6Z8Ax0jxMFEHLqsAJ1xQk4URRClXWLVywsQ5EBW4dFCJXdkGQVeZWzJvHE5uhhHBHbxEfw=="};
} // namespace webfront::developer::release
