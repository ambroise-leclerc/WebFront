#include <WebFront.hpp>
#include WEBFRONT_LIFE_ASSETS_HEADER
#include "../src/Life.hpp"
#include "../test/TemporaryDirectory.hpp"

#include <atomic>
#include <iostream>

namespace {
constexpr auto uiScript = R"JS(
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
        const packed = new Uint8Array(128);
        for (const i of [330, 331, 332]) packed[i >> 3] |= 1 << (i & 7);
        const next = await webFront.cppFunction('life.step')(packed);
        if (!(next instanceof Uint8Array) || next.length !== 128) throw new Error('Expected 128 packed Life bytes');
        const expected = new Uint8Array(128);
        for (const i of [299, 331, 363]) expected[i >> 3] |= 1 << (i & 7);
        if (!next.every((byte, i) => byte === expected[i])) throw new Error('Incorrect packed Life bit order');
        for (const value of [new Uint8Array(127), new Int8Array(128), '0'.repeat(1024)]) {
            const rejected = await webFront.cppFunction('life.step')(value).then(() => false, () => true);
            if (!rejected) throw new Error('Malformed Life grid accepted');
        }
        field('pattern-blinker').click();
        await waitFor(() => field('population').textContent === '3', 'Embedded blinker');
        field('step').click();
        await waitFor(() => field('generation').textContent === '0001', 'Embedded C++ generation');
        if (field('population').textContent !== '3') throw new Error('Embedded blinker population');
        field('play').click();
        await waitFor(() => Number(field('generation').textContent) >= 3, 'Embedded Life playback');
        field('clear').click();
        await delay(400);
        if (field('generation').textContent !== '0000' || field('population').textContent !== '0')
            throw new Error('Embedded Life reset retained a stale generation');
        const developmentDisabled = await webFront.cppFunction('webfront.developer.config')().then(() => false, () => true);
        if (!developmentDisabled) throw new Error('Distribution enabled developer callbacks');
        await webFront.cppFunction('test.report')(true, 'Embedded React + C++ Life passed with an empty working directory');
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
            BasicWF<webfront::NetProvider, webfront::fs::DefaultFSWith<webfront::fs::LifeUI>, webfront::http::DefaultBuffersPolicy, WEBFRONT_TEST_FRONTEND>;
        App               app;
        std::atomic<bool> passed{false};
        app.cppFunction("life.step", &webfront::demo::nextLife);
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
