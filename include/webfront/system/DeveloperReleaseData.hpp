#pragma once

// Pinned developer distribution. Regenerate with js/developer/tools/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.5.0"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.5.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-TV6S61kUhnCJc99/Ow890Pro47mfZ0fBqki17lx0xnpCNmcfcqZPpO1SG09Su50WIEtRDopMxt3yvanFwbLEdQ=="};
} // namespace webfront::developer::release
