/// @date 12/05/2023 13:20:27
/// @author Ambroise Leclerc
/// @brief Mocks

#include <system/FileSystem.hpp>

#include <array>
#include <filesystem>
#include <list>
#include <optional>
#include <string_view>


struct NoNames {
    static inline std::list<std::string_view> names{};
};

template<typename KnownFiles = NoNames>
struct MockFileSystem {
    MockFileSystem(std::filesystem::path docRoot) { root = docRoot; };
    struct Data {
        static constexpr std::array<uint64_t, 1> data{0};
        static constexpr size_t dataSize{0};
        static constexpr std::string_view encoding{"br"};
    };

    std::optional<webfront::fs::File> open(std::filesystem::path file) {
        auto filename = file.relative_path().string();
        if (find(KnownFiles::names.cbegin(), KnownFiles::names.cend(), filename) != KnownFiles::names.cend()) {
            openingsCounter++;
            return webfront::fs::File{Data{}};
        }
        return {};
    }

    inline static auto openingsCounter = 0;
    inline static std::filesystem::path root;
};
