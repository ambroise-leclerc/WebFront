// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Scenarios of a project that consumes the public webfront::bdd framework.
#include <webfront/testing/Bdd.hpp>

using namespace webfront::bdd;

const Scenario addition{"Addition", [] {
                            int sum = 0;
                            given("two numbers", [&] {
                                when("they are added", [&] {
                                    sum = 2 + 3;
                                    then("the sum is five", [&] {
                                        require(sum == 5);
                                    });
                                });
                            });
                        }};

const Scenario text{"A [bracketed; title] stays whole", [] {
                        given("a string", [] {
                            then("it has a length", [] {
                                check(std::string{"abc"}.size() == 3);
                            });
                        });
                    }};

const Scenario third{"Third", [] {}};

int main(int argc, char** argv) {
    return run(argc, argv);
}
