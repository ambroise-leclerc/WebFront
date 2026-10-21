#include <WebFront.hpp>
#include WEBFRONT_PI_ASSETS_HEADER
#include "../src/Pi.hpp"
#include "../test/TemporaryDirectory.hpp"

#include <atomic>
#include <iostream>

namespace {
constexpr auto uiScript = R"JS(
(async () => {
    try {
        const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
        for (let attempt = 0; !document.querySelector('button'); ++attempt) {
            if (attempt === 100) throw new Error('Embedded React interface did not mount');
            await delay(50);
        }
        document.querySelector('button').click();
        for (let attempt = 0; !document.querySelector('output').textContent.startsWith('3.14159265358979323846264338327950288419716939937510'); ++attempt) {
            if (attempt === 100) throw new Error('Embedded interface did not display PI');
            await delay(50);
        }
        const developmentDisabled = await webFront.cppFunction('webfront.developer.config')().then(() => false, () => true);
        if (!developmentDisabled) throw new Error('Distribution enabled developer callbacks');
        await webFront.cppFunction('test.report')(true, 'Embedded React + PI passed with an empty working directory');
    } catch (error) {
        await webFront.cppFunction('test.report')(false, String(error.stack || error));
    } finally { setTimeout(() => globalThis.close(), 0); }
})().catch(error => { console.error(error); globalThis.close(); });
)JS";
}  // namespace

int main() {
    try {
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        webfront::test::TemporaryDirectory empty;
        const auto                         original = std::filesystem::current_path();
        std::filesystem::current_path(empty.path());
        using App = webfront::
            BasicWF<webfront::NetProvider, webfront::fs::DefaultFSWith<webfront::fs::PiUI>, webfront::http::DefaultBuffersPolicy, WEBFRONT_TEST_FRONTEND>;
        App               app;
        std::atomic<bool> passed{false};
        app.cppFunction("pi", &webfront::demo::pi);
        app.cppFunction("test.report", [&](bool success, std::string message) {
            std::cout << message << std::endl;
            passed.store(success);
        });
        auto subscription = app.subscribeUIStarted([](App::UI ui) {
            ui.addScript(uiScript);
        });
        app.show();
        std::filesystem::current_path(original);
        return passed.load() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
