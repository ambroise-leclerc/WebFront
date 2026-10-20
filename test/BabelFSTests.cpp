#include "EmbeddedAssetChecks.hpp"
#include <system/BabelFS.hpp>

using namespace webfront;

SCENARIO("BabelFS provides the pinned Babel 8 standalone compiler") {
    auto path = std::filesystem::path("babel-standalone@8.0.7") / "babel.min.js";
    path.make_preferred();
    auto file = fs::BabelFS::open(path);
    REQUIRE(file.has_value());
    readEmbeddedAsset(*file, fs::generated::BabelData::dataSize);
    REQUIRE_FALSE(fs::BabelFS::open("babel-standalone@7.22.5/babel.min.js"));
}
