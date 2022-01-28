#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

using namespace std;
using nlohmann::json;

class SourcesDir;

int main(int argc, char** argv) {
    cxxopts::Options options("libgen", "Single Header generator");
    options.add_options()
        ("config", "json config file", cxxopts::value<std::string>()->default_value("./libgen.json"))
        ("h, help", "Print usage")
        ("sources_path", "Sources directory path", cxxopts::value<std::string>()->default_value("."));
    options.positional_help("<Sources directory path>");
    options.parse_positional({"sources_path"});
    

    try { 
        auto cli = options.parse(argc, argv);
    
        if (cli.count("help")) {
            cout << options.help() << "\n";
            return EXIT_SUCCESS;
        }

        auto sourcesDir = std::make_unique<SourcesDir>(cli["sources_path"].as<string>());

    }
    catch (std::exception& e) {
        cerr << "Error parsing command line arguments : " << e.what() << "\n";
        cerr << options.help() << "\n";
        return EXIT_FAILURE;
    }


    return EXIT_SUCCESS;
}

class SourcesDir {
public:
    SourcesDir(std::string_view path) {
        cout << "Will parse " << path << "\n";
    }
};