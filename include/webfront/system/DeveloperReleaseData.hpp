#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.5"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.6.5/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-qaSYOJvxcXtKIbrjfjZFFvfoQrda5NfBq24ttQ7SdnIN12tXFDYXRVe+JkrUHXZY6KOhF7qxmo4k8NOqLOupPA=="};
} // namespace webfront::developer::release
