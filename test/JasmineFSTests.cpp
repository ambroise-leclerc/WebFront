#include <catch2/catch_test_macros.hpp>
#include <system/JasmineFS.hpp>

#include <array>
#include <string_view>
#include <utility>
#include <vector>

using namespace webfront;

SCENARIO("JasmineFS provides Jasmine testing library") {
    GIVEN("A JasmineFS") {
        constexpr std::array<std::pair<std::string_view, std::size_t>, 6> assets{
            {{"jasmine_favicon.png", 1290}, {"jasmine.css", 7322}, {"jasmine.js", 61551}, {"jasmine-html.js", 6057}, {"boot0.js", 1363}, {"boot1.js", 1926}}
        };
        for (const auto& [name, size] : assets) {
            WHEN("requesting " << name) {
                auto path = std::filesystem::path("jasmine/4.6.0") / name;
                path.make_preferred();
                auto file = fs::JasmineFS::open(path);
                THEN("the gzip payload and advertised size are returned") {
                    REQUIRE(file.has_value());
                    REQUIRE(file->isEncoded());
                    REQUIRE(file->getEncoding() == "gzip");
                    std::vector<char> buffer(size + 1);
                    REQUIRE(file->read(buffer) == size);
                    REQUIRE(static_cast<unsigned char>(buffer[0]) == 0x1f);
                    REQUIRE(static_cast<unsigned char>(buffer[1]) == 0x8b);
                }
            }
        }
    }
}
