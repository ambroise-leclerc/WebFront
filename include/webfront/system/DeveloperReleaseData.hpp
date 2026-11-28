#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"1.2.1"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/1.2.1/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-FeBZ0XcRqn3OJUJpwtwqzXUey7gQHploPP6H29bj4K0Tbks1Ks5s8Cv9Ioo/chDwcmdWmWXn/iRZqjlBuLVkYw=="};
} // namespace webfront::developer::release
