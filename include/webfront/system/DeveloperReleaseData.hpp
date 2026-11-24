#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"1.2.0"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/1.2.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-cD2G+jM1i08t77RG6vCDbKskAjVyshtrKYRDByja272r0s2hVVFZxI8nbX8LGed1WtoiN8WPfitCjjIzRFP9XA=="};
} // namespace webfront::developer::release
