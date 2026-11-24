// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-20
//
// WebFront — tests/unit
// Role: The Game of Life rules of the examples.
// Thread: worker-safe | Alloc: heap-init
// LLM: pure computation tests

#include "../../examples/life/Life.hpp"
#include "../../examples/life/LifeDeveloperOptions.hpp"
#include "TemporaryDirectory.hpp"
#include <webfront/testing/Bdd.hpp>

#include <fstream>

using namespace webfront::bdd;

using webfront::demo::LifeGrid;
using webfront::demo::nextLife;

const Scenario lifeGenerations{"Life generations follow B3/S23 synchronously", [] {
                                   given("an empty grid and a stable block", [&] {
                                       require(nextLife({}).none());
                                       LifeGrid block;
                                       for (const auto i : {330, 331, 362, 363})
                                           block.set(static_cast<std::size_t>(i));
                                       require(nextLife(block) == block);
                                   });
                                   given("a horizontal blinker", [&] {
                                       LifeGrid horizontal, vertical;
                                       for (const auto i : {330, 331, 332})
                                           horizontal.set(static_cast<std::size_t>(i));
                                       for (const auto i : {299, 331, 363})
                                           vertical.set(static_cast<std::size_t>(i));
                                       when("two generations are computed", [&] {
                                           require(nextLife(horizontal) == vertical);
                                           require(nextLife(vertical) == horizontal);
                                           require(horizontal.count() == 3);
                                       });
                                   });
                                   given("a glider", [&] {
                                       LifeGrid grid, translated;
                                       for (const auto i : {1, 34, 64, 65, 66})
                                           grid.set(static_cast<std::size_t>(i));
                                       for (const auto i : {34, 67, 97, 98, 99})
                                           translated.set(static_cast<std::size_t>(i));
                                       for (int i = 0; i < 4; ++i)
                                           grid = nextLife(grid);
                                       require(grid == translated);
                                   });
                               }};

const Scenario lifeWraps{"Life wraps both axes without treating adjacent rows as neighbors", [] {
                             LifeGrid horizontal, vertical;
                             for (const auto i : {31, 0, 1})
                                 horizontal.set(static_cast<std::size_t>(i));
                             for (const auto i : {992, 0, 32})
                                 vertical.set(static_cast<std::size_t>(i));
                             require(nextLife(horizontal) == vertical);
                             require(nextLife(vertical) == horizontal);
                             LifeGrid isolated;
                             for (const auto i : {30, 31, 34})
                                 isolated.set(static_cast<std::size_t>(i));
                             require(nextLife(isolated).none());
                         }};

using webfront::demo::parseLifeDeveloperOptions;
using webfront::demo::prepareLifeProject;

const Scenario webFrontLifeDeveloperStarts{
    "WebFrontLifeDeveloper starts without arguments",
    [] {
        given("an empty command line", [&] {
            const char* argv[]  = {"WebFrontLifeDeveloper"};
            const auto  options = parseLifeDeveloperOptions(1, argv);
            then("the project is life-project and the pinned environment is used", [&] {
                require(options);
                require(options->project == "life-project");
                require(!options->environment);
                require(!options->integrity);
            });
        });
        given("an explicit project", [&] {
            const char* argv[]  = {"WebFrontLifeDeveloper", "other"};
            const auto  options = parseLifeDeveloperOptions(2, argv);
            then("it replaces the default and keeps the pinned environment", [&] {
                require(options);
                require(options->project == "other");
                require(!options->environment);
            });
        });
        given("a project with an environment archive and its integrity", [&] {
            const char* argv[]  = {"WebFrontLifeDeveloper", "other", "https://example.test/environment.tgz", "sha512-abc"};
            const auto  options = parseLifeDeveloperOptions(4, argv);
            then("both are passed on", [&] {
                require(options);
                require(options->environment == "https://example.test/environment.tgz");
                require(options->integrity == "sha512-abc");
            });
        });
        given("an incomplete or empty command line", [&] {
            const char* three[] = {"WebFrontLifeDeveloper", "other", "https://example.test/environment.tgz"};
            const char* empty[] = {"WebFrontLifeDeveloper", ""};
            const char* many[]  = {"WebFrontLifeDeveloper", "a", "b", "c", "d"};
            then("it is rejected", [&] {
                require(!parseLifeDeveloperOptions(3, three));
                require(!parseLifeDeveloperOptions(2, empty));
                require(!parseLifeDeveloperOptions(5, many));
            });
        });
    }};

const Scenario defaultLife{"The default Life project is created once and then reused", [] {
                               webfront::test::TemporaryDirectory work;
                               const auto                         project = work.path() / "life-project";
                               given("a missing project directory", [&] {
                                   prepareLifeProject(project);
                                   then("it exists", [&] {
                                       require(std::filesystem::is_directory(project));
                                   });
                                   when("the user has edited a file and the example starts again", [&] {
                                       std::ofstream{project / "webfront.project.json"} << "edited";
                                       prepareLifeProject(project);
                                       then("the content is unchanged", [&] {
                                           std::ifstream in{project / "webfront.project.json"};
                                           std::string   content;
                                           std::getline(in, content);
                                           require(content == "edited");
                                       });
                                   });
                               });
                               given("a path occupied by a regular file", [&] {
                                   std::ofstream{work.path() / "occupied"} << "x";
                                   then("preparation fails so that the example exits with an error", [&] {
                                       requireThrows<std::filesystem::filesystem_error>([&] {
                                           prepareLifeProject(work.path() / "occupied");
                                       });
                                   });
                               });
                           }};
