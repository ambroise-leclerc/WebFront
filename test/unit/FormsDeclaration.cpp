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
