#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.9.1"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.9.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-XMCnzxOcCPQF8JOefi2vsFfOcb41gX1avVwqF45431UZ0c4sIhwYZ+M3OryPKNuD6wM1PK7/UTMqV5uWFSPCxw=="};
} // namespace webfront::developer::release
