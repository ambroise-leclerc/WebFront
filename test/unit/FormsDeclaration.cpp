// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-30
//
// WebFront — tests/unit
// Role: Writes the TypeScript declaration of the hand-described settings to the file given as argument.
// Thread: worker-safe | Alloc: heap-init
// LLM: tool of the developer journey; wf check type-checks its output

// Writes the TypeScript declaration of the hand-described settings (#383) to the file given as argument: the developer
// journey type-checks with wf check a module of its project that imports it.

#include "FormsSettings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: forms_declaration <output.d.ts>\n";
        return 64;
    }
    const std::filesystem::path output = argv[1];
    if (output.has_parent_path())
        std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::binary);
    file << "// Generated from the C++ description of Settings: do not edit.\n" << webfront::forms::typescriptDeclaration(forms_fixture::settingsDescription());
    if (!file.flush()) {
        std::cerr << "cannot write " << output << "\n";
        return 1;
    }
}
