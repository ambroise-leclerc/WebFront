/// @date 17/02/2023 23:27:42
/// @author Ambroise Leclerc
/// @brief File systems providing access to real or virtual file systems
#pragma once


namespace webfront {

template <typename T>
concept FileSystem = requires(T fs) {
    ts.read(std::string)->std::vector<std::byte>;
    ts.write(std::string, std::vector<std::byte>);
}

namespace filesystem {

// A virtual filesystem with a generated index.html, 
class DefaultFileSystem {

};


}} // namespace webfront::filesystem
