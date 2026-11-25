// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-23
//
// WebFront — tests/cmake
// Role: Scenarios of a project that consumes the public webfront::bdd framework.
// Thread: ui-loop | Alloc: unrestricted
// LLM: consumer fixture; verifies the installed Bdd interface, part by part, from another project

#include <webfront/testing/bdd/Approx.hpp>
#include <webfront/testing/bdd/Core.hpp>
#include <webfront/testing/bdd/Runner.hpp>

using namespace webfront::bdd;

const Scenario addition{"Addition", [] {
                            int sum = 0;
                            given("two numbers", [&] {
                                when("they are added", [&] {
                                    sum = 2 + 3;
                                    then("the sum is five", [&] {
                                        requireEqual(sum, 5);
                                        require(sum / 3.0 == approx(1.6667).epsilon(1e-4));
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

const Scenario third{"Third", [] { }};

int main(int argc, char** argv) {
    return run(argc, argv);
}
