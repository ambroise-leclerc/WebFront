#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.3"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.6.3/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-3AVF2xKsUfHys5VREA6DmkjiEDKjx9VQtPTrsjdiazzfpNQvbKEjW2ox5Kd2hPeOP5MfcMvbDHQ4SZoO97P5Xg=="};
} // namespace webfront::developer::release
