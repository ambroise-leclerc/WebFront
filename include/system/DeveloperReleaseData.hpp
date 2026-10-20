#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.3.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.3.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-l2tCLMD7uDDfESVU9BS0/khpwAtO6nqWyeA1Ze5RtctxCCZJjcudLWv8tybJnJgnYva0CRSmnTIhudTQew5QKQ=="};
} // namespace webfront::developer::release
