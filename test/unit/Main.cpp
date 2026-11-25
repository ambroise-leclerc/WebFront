// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-14
//
// WebFront — tests/unit
// Role: Runs the scenarios of the unit tests (webfront/testing/bdd/Runner.hpp).
// Thread: worker-safe | Alloc: heap-init
// LLM: entry point; tests --help lists the filters

#include <webfront/testing/bdd/Runner.hpp>

int main(int argc, char** argv) {
    return webfront::bdd::run(argc, argv);
}
