#include <WebFront.hpp>

#include <system/FileSystem.hpp>
#include <system/IndexFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/NativeFS.hpp>
#include <tooling/Logger.hpp>
#include <tooling/PathUtils.hpp>

#include <atomic>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std;
using namespace webfront;

namespace {

constexpr string_view cppToJsToken{"cpp-to-js-token"};
constexpr string_view jsToCppToken{"js-to-cpp-token"};

struct TestState {
    atomic<bool> browserReady{false};
    atomic<bool> jsToCppObserved{false};
    atomic<bool> jasmineReported{false};
    atomic<bool> passed{false};
};

using TestFS = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::JasmineFS>;
using TestWF = BasicWF<NetProvider, TestFS>;

class BrowserIntegrationTest {
public:
    explicit BrowserIntegrationTest(const filesystem::path& docRoot)
        : webFront("9002", docRoot) {
        registerCallbacks();
    }

    int run() {
        log::info("Starting automated Jasmine browser integration test");
        webFront.openAndRun("SpecRunner.html");
        return result();
    }

private:
    TestWF webFront;
    TestState state;
    optional<TestWF::UI> connectedUI;

    void registerCallbacks() {
        webFront.onUIStarted([this](TestWF::UI ui) {
            connectedUI.emplace(ui);
        });
        webFront.cppFunction<void>("browserReady", [this] {
            browserReady();
        });
        webFront.cppFunction<void, string>("recordFromJs", [this](const string& token) {
            recordFromJs(token);
        });
        webFront.cppFunction<void, string>("reportJasmine", [this](const string& overallStatus) {
            reportJasmine(overallStatus);
        });
    }

    void browserReady() {
        state.browserReady = true;
        requireUI().jsFunction("webfrontTests.receiveFromCpp")(cppToJsToken);
    }

    void recordFromJs(const string& token) {
        state.jsToCppObserved = token == jsToCppToken;
    }

    void reportJasmine(const string& overallStatus) {
        state.jasmineReported = true;
        state.passed = overallStatus == "passed" && state.browserReady && state.jsToCppObserved;
        requireUI().jsFunction("webfrontTests.close")(state.passed.load());
    }

    TestWF::UI& requireUI() {
        if (!connectedUI)
            throw runtime_error("Browser callback received without a connected UI");
        return *connectedUI;
    }

    int result() const {
        if (!state.jasmineReported) {
            log::error("Jasmine did not report a result");
            return 1;
        }
        if (!state.passed) {
            log::error("Jasmine or one of the bridge-direction checks failed");
            return 1;
        }

        log::info("Automated Jasmine browser integration test passed");
        return 0;
    }
};

int runBrowserIntegration() {
    const auto docRoot = tooling::findTestRoot("SpecRunner.html");
    BrowserIntegrationTest test(docRoot);
    return test.run();
}

}  // namespace

int main() {
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);

    try {
        return runBrowserIntegration();
    } catch (const exception& error) {
        log::error("Browser integration test failed: {}", error.what());
        cerr << error.what() << '\n';
        return 1;
    }
}
