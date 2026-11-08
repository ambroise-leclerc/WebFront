#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.5"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.6.5/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-n5ZamtC36dx0jYUjs4hjfxiXhFzKnHdDwcDXI21T0OQ/fUX8BjhZX/j5G4Q80XogUJwL0hag8hk/HqzKBDAu3g=="};
} // namespace webfront::developer::release
