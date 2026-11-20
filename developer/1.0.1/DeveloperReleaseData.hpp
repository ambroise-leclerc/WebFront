#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"1.0.1"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/1.0.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-h8cCC0w4Zieis7kvxE4BP22Kd3k1cbMnGBTI1+m663O9r7J7Ru/Dsm/VFvyyv/dg1PNi+6NLBqCL3S2lL+30dQ=="};
} // namespace webfront::developer::release
