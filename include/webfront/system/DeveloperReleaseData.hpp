#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.2"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.6.2/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-SYnS6iRdK0VZM1PLECwK9Iwm/I6wxVlpM+1a0IXDs/lZN0y7lSB6hnv4+gSsX+U+aZ600WDXniF4w0LtHJFH/A=="};
} // namespace webfront::developer::release
