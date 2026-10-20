#include "../src/Pi.hpp"
#include "../test/TemporaryDirectory.hpp"
#include <WebFront.hpp>

#include <atomic>
#include <iostream>

namespace {
class FixturesFS {
public:
    explicit FixturesFS(std::filesystem::path root) : root(std::filesystem::canonical(root)) { }
    std::optional<webfront::fs::File> open(std::filesystem::path path) const {
        const auto relative = path.relative_path().generic_string();
        if (!relative.starts_with("fixtures/"))
            return {};
        std::ifstream stream(webfront::developer::projectPath(root, relative), std::ios::binary);
        if (stream)
            return webfront::fs::File{std::move(stream)};
        return {};
    }

private:
    std::filesystem::path root;
};
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::invalid_argument("Expected fixture directory and header output directory");
        // Frontend construction performs CEF subprocess dispatch before temporary files are created.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        webfront::test::TemporaryDirectory project;
        std::filesystem::copy(argv[1], project.path() / "fixtures", std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(project.path() / "fixtures/main.tsx", project.path() / "main.tsx");
        std::filesystem::copy_file(project.path() / "fixtures/style.css", project.path() / "style.css");
        std::string            integrity;
        const auto             config = project.path() / "fixtures/provision.json";
        std::ifstream          provisioning(config);
        const std::string      json{std::istreambuf_iterator<char>(provisioning), {}};
        webfront::json::Reader reader{json};
        reader.object([&](const std::string& key) {
            if (key == "integrity")
                integrity = reader.string();
            else
                reader.skipValue();
        });
        using App = webfront::BasicDeveloperApp<WEBFRONT_TEST_FRONTEND, FixturesFS>;
        App               app{project.path()};
        std::atomic<bool> passed{false};
        app.cppFunction("pi", &webfront::demo::pi);
        app.cppFunction("test.header", [&] {
            std::ifstream     file(project.path() / "generated/PiUI.hpp", std::ios::binary);
            const std::string header{std::istreambuf_iterator<char>(file), {}};
            // The generated header is larger than a bridge string: return a stable small digest.
            return std::to_string(std::hash<std::string>{}(header));
        });
        app.cppFunction("test.report", [&](bool success, std::string message) {
            std::cout << message << std::endl;
            passed.store(success);
        });
        app.bootstrap("developer", {.environment = "/fixtures/environment.tgz", .integrity = integrity, .registry = "/fixtures/registry/"});
        auto subscription = app.subscribeUIStarted([injected = false](App::UI ui) mutable {
            if (injected)
                return;
            injected = true;
            ui.addScript(
                "import('/fixtures/run.mjs').catch(async e => { await webFront.cppFunction('test.report')(false, String(e.stack || e)); window.close(); });");
        });
        app.show();
        if (!passed.load())
            return 1;
        std::filesystem::create_directories(argv[2]);
        std::filesystem::copy_file(project.path() / "generated/PiUI.hpp",
                                   std::filesystem::path(argv[2]) / "PiUI.hpp",
                                   std::filesystem::copy_options::overwrite_existing);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
