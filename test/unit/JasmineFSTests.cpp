#include "EmbeddedAssetChecks.hpp"
#include <webfront/system/JasmineFS.hpp>

#include <array>
#include <string_view>
#include <utility>

using namespace webfront;

SCENARIO("JasmineFS provides the pinned Jasmine 7 distribution") {
    constexpr std::array<std::pair<std::string_view, std::size_t>, 5> assets{
        {
         {"jasmine.js", fs::generated::JasmineJsData::dataSize},
         {"jasmine-html.js", fs::generated::JasmineHtmlData::dataSize},
         {"jasmine.css", fs::generated::JasmineCssData::dataSize},
         {"boot.js", fs::generated::JasmineBootData::dataSize},
         {"jasmine_favicon.png", fs::generated::JasmineFaviconData::dataSize},
         }
    };
    for (const auto& [name, size] : assets) {
        WHEN("requesting " << name) {
            auto path = std::filesystem::path("jasmine/7.0.2") / name;
            path.make_preferred();
            auto file = fs::JasmineFS::open(path);
            REQUIRE(file.has_value());
            const auto content = readEmbeddedAsset(*file, size, name != "jasmine_favicon.png");
            if (name == "jasmine_favicon.png")
                REQUIRE(content.substr(0, 8) == "\x89PNG\r\n\x1a\n");
        }
    }
    REQUIRE_FALSE(fs::JasmineFS::open("jasmine/7.0.2/boot0.js"));
    REQUIRE_FALSE(fs::JasmineFS::open("jasmine/4.6.0/jasmine.js"));
}
