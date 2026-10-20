#include "EmbeddedAssetChecks.hpp"
#include <system/ReactFS.hpp>

using namespace webfront;

SCENARIO("ReactFS provides the local React 19 DOM and automatic JSX runtime") {
    GIVEN("the pinned React runtime route") {
        auto file = fs::ReactFS::open("/react@19.3.1/runtime.mjs");
        REQUIRE(file.has_value());
        readEmbeddedAsset(*file, fs::generated::ReactRuntimeData::dataSize);
    }
    THEN("obsolete UMD and unknown module routes are not served") {
        REQUIRE_FALSE(fs::ReactFS::open("react@18/umd/react.production.min.js"));
        REQUIRE_FALSE(fs::ReactFS::open("react@19.3.1/missing.mjs"));
    }
}
