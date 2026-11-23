// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Scenarios of a project that embeds WebFront and tests with webfront::bdd.
#include <webfront/testing/Bdd.hpp>

using namespace webfront::bdd;

const Scenario embedded{"Embedded WebFront", [] {
                            given("a scenario", [] {
                                then("it runs", [] {
                                    require(true);
                                });
                            });
                        }};

int main(int argc, char** argv) {
    return run(argc, argv);
}
