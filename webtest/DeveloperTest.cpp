#include "../src/Life.hpp"
#include "../src/Pi.hpp"
#include "../test/TemporaryDirectory.hpp"
#include <WebFront.hpp>

#include <atomic>
#include <iostream>

namespace {
std::string readFixtureText(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 16 * 1024 * 1024)
        throw std::length_error("Fixture text exceeds its byte limit");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read fixture text");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty() && !file.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Truncated fixture text");
    return text;
}

class FixturesFS {
public:
    explicit FixturesFS(std::filesystem::path projectRoot) : root(std::filesystem::canonical(projectRoot)) { }
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
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        if (argc != 3 && argc != 4)
            throw std::invalid_argument("Expected fixture directory, header output directory and optional --public");
        webfront::test::TemporaryDirectory project;
        std::filesystem::copy(argv[1], project.path() / "fixtures", std::filesystem::copy_options::recursive);
        std::string            integrity;
        const auto             config = project.path() / "fixtures/provision.json";
        const std::string      json   = readFixtureText(config);
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
        app.cppFunction("life.step", &webfront::demo::lifeStep);
        app.cppFunction("test.header", [&](std::string name) {
            if (name != "PiUI" && name != "LifeUI")
                throw std::invalid_argument("Unknown generated fixture");
            const auto header = readFixtureText(project.path() / "generated" / (name + ".hpp"));
            // The generated header is larger than a bridge string: return a stable small digest.
            return std::to_string(std::hash<std::string>{}(header));
        });
        app.cppFunction("test.report", [&](bool success, std::string message) {
            std::cout << message << std::endl;
            passed.store(success);
        });
        if (argc == 4) {
            if (std::string_view(argv[3]) != "--public")
                throw std::invalid_argument("Unknown smoke-test mode");
            app.bootstrap("developer");
        } else {
            app.bootstrap("developer", {.environment = "/fixtures/environment.tgz", .integrity = integrity, .registry = "/fixtures/registry/"});
        }
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
        for (const auto* name : {"PiUI.hpp", "LifeUI.hpp"})
            std::filesystem::copy_file(project.path() / "generated" / name,
                                       std::filesystem::path(argv[2]) / name,
                                       std::filesystem::copy_options::overwrite_existing);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
