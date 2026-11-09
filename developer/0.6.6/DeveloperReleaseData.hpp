#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.6"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/0.6.6/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-sRRxjGZWO4vGNZn0lUw/MPWGx7lV/5WMyITacXpDH3cGRvhCEZJC7p8lLYfjmRn5kBuepfaU40txLJwA5KDbZQ=="};
} // namespace webfront::developer::release
