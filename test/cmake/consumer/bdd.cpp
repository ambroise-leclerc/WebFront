// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-23
//
// WebFront — tests/cmake
// Role: Scenarios of a project that embeds WebFront and tests with webfront::bdd.
// Thread: ui-loop | Alloc: unrestricted
// LLM: consumer fixture of the portability tier

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
