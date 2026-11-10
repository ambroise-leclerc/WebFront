#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.9.0"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.9.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-bBETj63LZ33WBNlcVp7Yt9X6D32iUsP7/UrHrW1cW6hNAjKYqjrGjPNqXkevFuPHtP2gpqJ5yf3DhqAwm3J38Q=="};
} // namespace webfront::developer::release
