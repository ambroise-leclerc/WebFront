#include <system/FileSystem.hpp>
#include <system/IndexFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/NativeFS.hpp>
#include <tooling/Logger.hpp>
#include <tooling/PathUtils.hpp>
#include <WebFront.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace std;
using namespace webfront;

namespace {

constexpr string_view cppToJsToken{"cpp-to-js-token"};
constexpr string_view jsToCppToken{"js-to-cpp-token"};

struct TestState {
    atomic<bool> browserReady{false};
    atomic<bool> jsToCppObserved{false};
    atomic<bool> jsArraysObserved{false};
    atomic<bool> jsTupleObserved{false};
    atomic<bool> jasmineReported{false};
    atomic<bool> passed{false};
};

using TestFS = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::JasmineFS>;
using TestWF = BasicWF<NetProvider, TestFS>;

class BrowserIntegrationTest {
public:
    explicit BrowserIntegrationTest(const filesystem::path& docRoot) : webFront("9002", docRoot) {
        registerCallbacks();
    }

    int run() {
        log::info("Starting automated Jasmine browser integration test");
        webFront.openAndRun("SpecRunner.html");
        return result();
    }

private:
    TestWF               webFront;
    TestState            state;
    optional<TestWF::UI> connectedUI;

    const array<uint8_t, 2>  cppU8{0, 255};
    const array<int8_t, 2>   cppI8{-128, 127};
    const array<uint16_t, 2> cppU16{0, 65535};
    const array<int16_t, 2>  cppI16{-32768, 32767};
    const array<uint32_t, 2> cppU32{0, 0xffffffffu};
    const array<int32_t, 2>  cppI32{numeric_limits<int32_t>::min(), numeric_limits<int32_t>::max()};
    const array<uint64_t, 2> cppU64{0, numeric_limits<uint64_t>::max()};
    const array<int64_t, 2>  cppI64{numeric_limits<int64_t>::min(), numeric_limits<int64_t>::max()};
    const array<float, 2>    cppFloat{-1.5F, 42.25F};
    const array<double, 2>   cppDouble{-1.5, 42.25};

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
        webFront.cppFunction<void, string, string>("reportJasmine", [this](const string& overallStatus, const string& failures) {
            reportJasmine(overallStatus, failures);
        });
        webFront.cppFunction<void,
                             vector<uint8_t>,
                             vector<int8_t>,
                             vector<uint16_t>,
                             vector<int16_t>,
                             vector<uint32_t>,
                             vector<int32_t>,
                             vector<uint64_t>,
                             vector<int64_t>,
                             vector<float>,
                             vector<double>>(
            "recordArraysFromJs",
            [this](const vector<uint8_t>&  u8,
                   const vector<int8_t>&   i8,
                   const vector<uint16_t>& u16,
                   const vector<int16_t>&  i16,
                   const vector<uint32_t>& u32,
                   const vector<int32_t>&  i32,
                   const vector<uint64_t>& u64,
                   const vector<int64_t>&  i64,
                   const vector<float>&    floats,
                   const vector<double>&   doubles) {
                state.jsArraysObserved = u8 == vector<uint8_t>{0, 255} && i8 == vector<int8_t>{-128, 127} && u16 == vector<uint16_t>{0, 65535}
                                         && i16 == vector<int16_t>{-32768, 32767} && u32 == vector<uint32_t>{0, 0xffffffffu}
                                         && i32 == vector<int32_t>{numeric_limits<int32_t>::min(), numeric_limits<int32_t>::max()}
                                         && u64 == vector<uint64_t>{0, numeric_limits<uint64_t>::max()}
                                         && i64 == vector<int64_t>{numeric_limits<int64_t>::min(), numeric_limits<int64_t>::max()}
                                         && floats == vector<float>{-1.5F, 42.25F} && doubles == vector<double>{-1.5, 42.25};
            });
        webFront.cppFunction<void, tuple<int, string>>("recordTupleFromJs", [this](const tuple<int, string>& value) {
            state.jsTupleObserved = value == tuple<int, string>{42, "tuple"};
        });
    }

    void browserReady() {
        state.browserReady = true;
        requireUI().jsFunction("webfrontTests.receiveFromCpp")(cppToJsToken);
        requireUI().jsFunction("webfrontTests.receiveArraysFromCpp")(cppU8, cppI8, cppU16, cppI16, cppU32, cppI32, cppU64, cppI64, cppFloat, cppDouble);
    }

    void recordFromJs(const string& token) {
        state.jsToCppObserved = token == jsToCppToken;
    }

    void reportJasmine(const string& overallStatus, const string& failures) {
        state.jasmineReported = true;
        state.passed          = overallStatus == "passed" && state.browserReady && state.jsToCppObserved && state.jsArraysObserved && state.jsTupleObserved;
        if (!failures.empty())
            log::error("Jasmine failures:\n{}", failures);
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
    const auto             docRoot = tooling::findTestRoot("SpecRunner.html");
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
