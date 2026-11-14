#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.9.3"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.9.3/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-fVr362da3+0iuX4WHvxg2Vp9nuZ4/qWJXkI1kU9MHfqoDYrN4HB9f3qvFm2p7NG7c3CuW/zHpiNsGmywm77PdA=="};
} // namespace webfront::developer::release
