#include "../unit/GzipFixtures.hpp"
#include "GzipZlib.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
std::size_t comparisons{};
void        check(std::string_view bytes, std::size_t limit = 4 * 1024 * 1024) {
    gziptest::compare(bytes, limit);
    ++comparisons;
}
void exercise(std::string_view bytes) {
    const auto expected = gziptest::reference(bytes, 4 * 1024 * 1024);
    if (!expected)
        throw std::runtime_error("expected a valid corpus member");
    check(bytes);
    check(bytes, expected->size());
    if (!expected->empty())
        check(bytes, expected->size() - 1);
    // Exhaust short streams; sample large stored/literal streams and every trailer byte.
    const auto stride = std::max<std::size_t>(1, bytes.size() / 128);
    for (std::size_t size = 0; size < bytes.size(); size += stride)
        check(bytes.substr(0, size));
    for (std::size_t size = bytes.size() - 8; size < bytes.size(); ++size)
        check(bytes.substr(0, size));
    for (std::size_t index = 0; index < bytes.size(); index += stride) {
        auto corrupt    = std::string(bytes);
        corrupt[index] ^= static_cast<char>(0x80);
        check(corrupt);
    }
    for (const auto suffix : {std::string("\0", 1), std::string("garbage"), std::string(gzipfixtures::empty)})
        check(std::string(bytes) + suffix);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        for (const auto bytes : {gzipfixtures::fixed, gzipfixtures::dynamic, gzipfixtures::optional, gzipfixtures::empty})
            exercise(bytes);
        check(std::string(gzipfixtures::dynamic) + std::string(gzipfixtures::optional));
        for (int index = 1; index < argc; ++index) {
            for (const auto& entry : std::filesystem::directory_iterator(argv[index])) {
                if (entry.path().extension() != ".gz")
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                if (!file)
                    throw std::runtime_error("cannot read corpus");
                const std::string bytes(std::istreambuf_iterator<char>(file), {});
                std::cout << entry.path().filename().string() << '\n';
                exercise(bytes);
            }
        }
        std::cout << comparisons << " differential comparisons passed (zlib " << zlibVersion() << ")\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
