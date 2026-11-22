#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"1.0.2"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/1.0.2/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-An03RE80keNQZnPBCng5lO1P/O5I+hQ/K7SEh1mL4bfyZLQs2g6QxiUi3rD+Yas14zm7gDdoUxTxHc/Wn+g/Xw=="};
} // namespace webfront::developer::release
