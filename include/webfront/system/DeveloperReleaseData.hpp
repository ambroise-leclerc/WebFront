#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.5"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.6.5/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-YrAsNGzU7tMo7CF2XKSGjjyQ1CxlwdWRD86zWCKikmNW3c/nfwh6i/6iunHRL0CHK26LfoIjmUn/qnxoSSy5fg=="};
} // namespace webfront::developer::release
