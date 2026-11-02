#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"0.6.4"};
inline constexpr std::string_view environment{"https://ambroise-leclerc.github.io/WebFront/developer/0.6.4/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-FJe6RqVjsLEKgfnSONDCbfk2wsGfkGzzgV0q33NIR+DYZpVvxakL/cxsVwi67eQwRuHI6ivlnV9D/oV92xTNCg=="};
} // namespace webfront::developer::release
