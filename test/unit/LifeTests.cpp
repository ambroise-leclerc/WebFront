#include "../../examples/life/Life.hpp"
#include "../../examples/life/LifeDeveloperOptions.hpp"
#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>

#include <fstream>

using webfront::demo::LifeGrid;
using webfront::demo::nextLife;

SCENARIO("Life generations follow B3/S23 synchronously") {
    GIVEN("an empty grid and a stable block") {
        REQUIRE(nextLife({}).none());
        LifeGrid block;
        for (const auto i : {330, 331, 362, 363})
            block.set(static_cast<std::size_t>(i));
        REQUIRE(nextLife(block) == block);
    }
    GIVEN("a horizontal blinker") {
        LifeGrid horizontal, vertical;
        for (const auto i : {330, 331, 332})
            horizontal.set(static_cast<std::size_t>(i));
        for (const auto i : {299, 331, 363})
            vertical.set(static_cast<std::size_t>(i));
        WHEN("two generations are computed") {
            REQUIRE(nextLife(horizontal) == vertical);
            REQUIRE(nextLife(vertical) == horizontal);
            REQUIRE(horizontal.count() == 3);
        }
    }
    GIVEN("a glider") {
        LifeGrid grid, translated;
        for (const auto i : {1, 34, 64, 65, 66})
            grid.set(static_cast<std::size_t>(i));
        for (const auto i : {34, 67, 97, 98, 99})
            translated.set(static_cast<std::size_t>(i));
        for (int i = 0; i < 4; ++i)
            grid = nextLife(grid);
        REQUIRE(grid == translated);
    }
}

SCENARIO("Life wraps both axes without treating adjacent rows as neighbors") {
    LifeGrid horizontal, vertical;
    for (const auto i : {31, 0, 1})
        horizontal.set(static_cast<std::size_t>(i));
    for (const auto i : {992, 0, 32})
        vertical.set(static_cast<std::size_t>(i));
    REQUIRE(nextLife(horizontal) == vertical);
    REQUIRE(nextLife(vertical) == horizontal);
    LifeGrid isolated;
    for (const auto i : {30, 31, 34})
        isolated.set(static_cast<std::size_t>(i));
    REQUIRE(nextLife(isolated).none());
}

using webfront::demo::parseLifeDeveloperOptions;
using webfront::demo::prepareLifeProject;

SCENARIO("WebFrontLifeDeveloper starts without arguments") {
    GIVEN("an empty command line") {
        const char* argv[] = {"WebFrontLifeDeveloper"};
        const auto  options = parseLifeDeveloperOptions(1, argv);
        THEN("the project is life-project and the pinned environment is used") {
            REQUIRE(options);
            REQUIRE(options->project == "life-project");
            REQUIRE_FALSE(options->environment);
            REQUIRE_FALSE(options->integrity);
        }
    }
    GIVEN("an explicit project") {
        const char* argv[] = {"WebFrontLifeDeveloper", "other"};
        const auto  options = parseLifeDeveloperOptions(2, argv);
        THEN("it replaces the default and keeps the pinned environment") {
            REQUIRE(options);
            REQUIRE(options->project == "other");
            REQUIRE_FALSE(options->environment);
        }
    }
    GIVEN("a project with an environment archive and its integrity") {
        const char* argv[] = {"WebFrontLifeDeveloper", "other", "https://example.test/environment.tgz", "sha512-abc"};
        const auto  options = parseLifeDeveloperOptions(4, argv);
        THEN("both are passed on") {
            REQUIRE(options);
            REQUIRE(options->environment == "https://example.test/environment.tgz");
            REQUIRE(options->integrity == "sha512-abc");
        }
    }
    GIVEN("an incomplete or empty command line") {
        const char* three[] = {"WebFrontLifeDeveloper", "other", "https://example.test/environment.tgz"};
        const char* empty[] = {"WebFrontLifeDeveloper", ""};
        const char* many[]  = {"WebFrontLifeDeveloper", "a", "b", "c", "d"};
        THEN("it is rejected") {
            REQUIRE_FALSE(parseLifeDeveloperOptions(3, three));
            REQUIRE_FALSE(parseLifeDeveloperOptions(2, empty));
            REQUIRE_FALSE(parseLifeDeveloperOptions(5, many));
        }
    }
}

SCENARIO("The default Life project is created once and then reused") {
    webfront::test::TemporaryDirectory work;
    const auto                         project = work.path() / "life-project";
    GIVEN("a missing project directory") {
        prepareLifeProject(project);
        THEN("it exists") {
            REQUIRE(std::filesystem::is_directory(project));
        }
        WHEN("the user has edited a file and the example starts again") {
            std::ofstream{project / "webfront.project.json"} << "edited";
            prepareLifeProject(project);
            THEN("the content is unchanged") {
                std::ifstream in{project / "webfront.project.json"};
                std::string   content;
                std::getline(in, content);
                REQUIRE(content == "edited");
            }
        }
    }
    GIVEN("a path occupied by a regular file") {
        std::ofstream{work.path() / "occupied"} << "x";
        THEN("preparation fails so that the example exits with an error") {
            REQUIRE_THROWS_AS(prepareLifeProject(work.path() / "occupied"), std::filesystem::filesystem_error);
        }
    }
}
