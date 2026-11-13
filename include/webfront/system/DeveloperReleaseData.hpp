#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.9.2"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.9.2/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-6p5NL7shQwnwcYp2n9Kc2iOQAVbKEAEdFAYLsIuSpy4svBGzj/Lu/xBec2u0Iit0Vj88+0Lj8xE9WX73+/m+Xw=="};
} // namespace webfront::developer::release
