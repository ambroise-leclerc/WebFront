#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{"1.0.0"};
inline constexpr std::string_view environment{"https://dist.webfront.fr/developer/1.0.0/environment.tgz"};
inline constexpr std::string_view integrity{"sha512-jZTAAPzQmKYoSJ6URWkD3ksB8FvsVCUhAYw+E4P4yAV94eW9qp5wO3p2KKrWYjFEUqiyPU+H8pryi0E/Dvd2Sw=="};
} // namespace webfront::developer::release
