#include <WebFront.hpp>
#include WEBFRONT_LIFE_ASSETS_HEADER
#include "../src/Life.hpp"
#include "../test/TemporaryDirectory.hpp"

#include <atomic>
#include <iostream>

int main() {
    try {
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        webfront::test::TemporaryDirectory empty;
        const auto                         original = std::filesystem::current_path();
        std::filesystem::current_path(empty.path());
        using App = webfront::
            BasicWF<webfront::NetProvider, webfront::fs::DefaultFSWith<webfront::fs::LifeUI>, webfront::http::DefaultBuffersPolicy, WEBFRONT_TEST_FRONTEND>;
        App               app;
        std::atomic<bool> passed{false};
        app.cppFunction("life.step", &webfront::demo::lifeStep);
        app.cppFunction("test.report", [&](bool success, std::string message) {
            std::cout << message << std::endl;
            passed.store(success);
        });
        auto subscription = app.subscribeUIStarted([](App::UI ui) {
            ui.addScript(R"JS(
(async () => {
    try {
        const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
        const field = name => document.querySelector(`[data-testid="${name}"]`);
        const waitFor = async (predicate, message) => {
            for (let attempt = 0; !predicate(); ++attempt) {
                if (attempt === 100) throw new Error(message);
                await delay(50);
            }
        };
        await waitFor(() => field('pattern-blinker'), 'Embedded Life interface did not mount');
        field('pattern-blinker').click();
        await waitFor(() => field('population').textContent === '3', 'Embedded blinker');
        field('step').click();
        await waitFor(() => field('generation').textContent === '0001', 'Embedded C++ generation');
        if (field('population').textContent !== '3') throw new Error('Embedded blinker population');
        const developmentDisabled = await webFront.cppFunction('webfront.developer.config')().then(() => false, () => true);
        if (!developmentDisabled) throw new Error('Distribution enabled developer callbacks');
        await webFront.cppFunction('test.report')(true, 'Embedded React + C++ Life passed with an empty working directory');
    } catch (error) {
        await webFront.cppFunction('test.report')(false, String(error.stack || error));
    } finally { setTimeout(() => globalThis.close(), 0); }
})().catch(error => { console.error(error); globalThis.close(); });
)JS");
        });
        app.show();
        std::filesystem::current_path(original);
        return passed.load() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
