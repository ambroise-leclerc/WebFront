#include <components/Terminal.hpp>
#include <components/ToolManager.hpp>
#include <system/BabelFS.hpp>
#include <system/FileSystem.hpp>
#include <system/IndexFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/NativeFS.hpp>
#include <system/PackageFS.hpp>
#include <system/ReactFS.hpp>
#include <system/TerminalFS.hpp>
#include <system/ToolManagerFS.hpp>
#include <tooling/Logger.hpp>
#include <tooling/PathUtils.hpp>
#include <WebFront.hpp>

#include "RejectedWebSocketServer.hpp"
#include "../src/SignalDemo.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
    atomic<bool> jsExtendedFrameObserved{false};
    atomic<bool> jsArraysObserved{false};
    atomic<bool> jsTupleObserved{false};
    atomic<bool> cppResultObserved{false};
    atomic<bool> senderResultsObserved{false};
    atomic<bool> senderCancellationObserved{false};
    atomic<bool> jasmineReported{false};
    atomic<bool> subscriberNotified{false};
    atomic<bool> passed{false};
};

/// Project directory of the tools installed by webtest/js/tools-specs.mjs: a fresh temporary
/// directory, removed by main(), so the test never writes into the source tree.
struct TestToolsProject {
    static filesystem::path get() {
        static const filesystem::path directory = [] {
            const auto unique = to_string(chrono::system_clock::now().time_since_epoch().count());
            auto       path   = filesystem::temp_directory_path() / ("webfront-webtest-tools-" + unique);
            filesystem::create_directories(path);
            return path;
        }();
        return directory;
    }
};

/// Search endpoint of the fake npm registry used by webtest/js/tools-specs.mjs; its packuments and
/// tarball are plain files under webtest/registry/ (see make_fixture.py), served by NativeDebugFS.
/// The server drops the query string before looking up files, so tools-specs.mjs checks the query
/// wf.mjs sends.
class FakeRegistrySearchFS {
public:
    explicit FakeRegistrySearchFS(filesystem::path docRoot) : root(std::move(docRoot)) {}

    optional<fs::File> open(filesystem::path file) const {
        const auto request = file.relative_path().generic_string();
        if (request != "registry/-/v1/search")
            return {};
        ifstream stream(root / "registry" / "search.json", ios::binary);
        if (!stream.is_open())
            return {};
        return fs::File{std::move(stream)};
    }

private:
    filesystem::path root;
};

// Serve the actual example page so the React regression exercises the shipped UI.
class ReactDemoFS {
public:
    explicit ReactDemoFS(filesystem::path root) : page(root.parent_path() / "src" / "react.html") {}

    optional<fs::File> open(filesystem::path file) const {
        if (file.relative_path().generic_string() != "demos/react.html")
            return {};
        ifstream stream(page, ios::binary);
        if (!stream)
            return {};
        return fs::File{std::move(stream)};
    }

private:
    filesystem::path page;
};

// Load the full Jasmine suite at the filename used by the WebView2 lifecycle regression.
// A missing escape for the literal hash would request a different file and never report a result.
class EncodedRunnerFS {
public:
    explicit EncodedRunnerFS(filesystem::path root) : page(root / "SpecRunner.html") { }

    optional<fs::File> open(filesystem::path file) const {
        const auto request = file.relative_path().generic_string();
        if (request != "encoded path #.html" && request != "a+b.html")
            return {};
        ifstream stream(page, ios::binary);
        if (!stream)
            return {};
        return fs::File{std::move(stream)};
    }

private:
    filesystem::path page;
};

using TestFS = fs::Multi<EncodedRunnerFS,
                         ReactDemoFS,
                         FakeRegistrySearchFS,
                         fs::NativeDebugFS,
                         fs::IndexFS,
                         fs::JasmineFS,
                         fs::ReactFS,
                         fs::BabelFS,
                         fs::TerminalFS,
                         fs::ToolManagerFS,
                         fs::BasicPackageFS<TestToolsProject>>;
// CMake chooses an embedded frontend; the same specs run under CEF/Xvfb and Windows WebView2.
#ifndef WEBFRONT_TEST_FRONTEND
    #define WEBFRONT_TEST_FRONTEND webfront::frontend::CEFFrontend
#endif
using TestWF = BasicWFWithFrontend<NetProvider, TestFS, WEBFRONT_TEST_FRONTEND>;

class BrowserIntegrationTest {
public:
    explicit BrowserIntegrationTest(const filesystem::path& docRoot) : webFront("9002", docRoot) {
        registerCallbacks();
        registerBitsetCallbacks();
        registerTerminalCommands();
    }

    ~BrowserIntegrationTest() {
        if (bitsetFutureThread.joinable())
            bitsetFutureThread.join();
        webFront.shutdown();
    }

#ifdef _MSC_VER
    // With an unavailable embedded frontend, openAndRun() unconditionally throws. MSVC's
    // inliner detects that and flags the return below as unreachable, uniquely among our compilers,
    // turning it into an error under /WX; see the matching suppression in include/WebFront.hpp.
    #pragma warning(push)
    #pragma warning(disable : 4702)
#endif
    int run() {
        log::info("Starting automated Jasmine browser integration test");
        // Exercise the same page-reference contract under both embedded frontends.
        webFront.openAndRun("encoded path %23.html?webfront=1#integration");
        return result();
    }
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

private:
    // WebFront dispatches CEF subprocesses during construction. Start the fixture's
    // networking thread only after that bootstrap has completed in the browser process.
    TestWF               webFront;
    RejectedWebSocketServer rejectedWebSocket;
    optional<TaskHandle>             cancelledTask;
    TestState            state;
    optional<TestWF::UI> connectedUI;
    UISubscription                   uiSubscription;
    future<string> cppResult;
    jthread bitsetFutureThread;
    static constexpr chrono::seconds cppResultWaitTimeout{10};

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

    static void expectBitsetReady(future<bitset<9>>& value, string_view context) {
        if (value.wait_for(cppResultWaitTimeout) != future_status::ready)
            throw logic_error(string(context) + " timed out");
    }

    static void expectBitset(future<bitset<9>>& value, const bitset<9>& expected) {
        expectBitsetReady(value, "Bitset future");
        if (value.get() != expected)
            throw logic_error("Bitset future returned incorrect bits");
    }

    static void expectRejected(future<bitset<9>>& value, int kind) {
        const auto context = "Malformed bitset future for case " + to_string(kind);
        expectBitsetReady(value, context);
        try {
            static_cast<void>(value.get());
        } catch (const runtime_error&) {
            return;
        }
        throw logic_error(context + " succeeded");
    }

    using BitsetFutures = array<future<bitset<9>>, 4>;

    void startBitsetFutureChecks(TestWF::UI ui) {
        // Resolve links and dispatch on the server context, alongside connection insertion.
        BitsetFutures values;
        values.front() = ui.jsFunction<bitset<9>>("webfrontTests.bitsetEcho")(bitset<9>{0x181}, true);
        for (size_t kind = 0; kind < values.size() - 1; ++kind)
            values[kind + 1] = ui.jsFunction<bitset<9>>("webfrontTests.bitsetBad")(kind, true);
        bitsetFutureThread = jthread([this, ui, values = std::move(values)]() mutable {
            checkBitsetFutures(ui, std::move(values));
        });
    }

    void checkBitsetFutures(TestWF::UI ui, BitsetFutures values) {
        bool passed = false;
        try {
            // Wait off the server context so it remains free to deliver any response order.
            expectBitset(values.front(), bitset<9>{0x181});
            for (size_t kind = 0; kind < values.size() - 1; ++kind)
                expectRejected(values[kind + 1], static_cast<int>(kind));
            passed = true;
        } catch (const exception& error) {
            log::error("Bitset future checks failed: {}", error.what());
        }
        webFront.spawn(reportBitsetFutureChecks(ui, passed));
    }

    Task reportBitsetFutureChecks(TestWF::UI ui, bool passed) {
        try {
            ui.jsFunction("webfrontTests.bitsetFutureChecksFinished")(passed);
        } catch (const exception& error) {
            log::error("Cannot report bitset future checks: {}", error.what());
        }
        co_return;
    }

    Task checkBitsetResults(TestWF::UI ui) {
        bool passed = false;
        try {
            const auto result = co_await ui.jsFunctionSender<bitset<9>>("webfrontTests.bitsetEcho")(bitset<9>{0x181});
            if (result != bitset<9>{0x181})
                throw logic_error("Bitset sender returned incorrect bits");
            for (int kind = 0; kind < 3; ++kind) {
                bool rejected = false;
                try {
                    co_await ui.jsFunctionSender<bitset<9>>("webfrontTests.bitsetBad")(kind);
                } catch (const runtime_error&) {
                    rejected = true;
                }
                if (!rejected)
                    throw logic_error("Malformed bitset sender succeeded for case " + to_string(kind));
            }
            passed = true;
        } catch (const exception& error) {
            log::error("Bitset sender checks failed: {}", error.what());
        }
        ui.jsFunction("webfrontTests.bitsetChecksFinished")(passed);
    }

    Task checkSenderResults(TestWF::UI ui) {
        const auto value = co_await ui.jsFunctionSender<string>("webfrontTests.senderValue")("from-sender");
        co_await ui.jsFunctionSender("webfrontTests.senderVoid")();
        bool exceptionObserved = false;
        try {
            co_await ui.jsFunctionSender<string>("webfrontTests.senderThrows")();
        } catch (const runtime_error& error) {
            exceptionObserved = string_view(error.what()) == "sender browser failure";
        }
        state.senderResultsObserved = value == "sender:from-sender" && exceptionObserved;
        ui.jsFunction("webfrontTests.senderChecksFinished")(state.senderResultsObserved.load());
    }

    struct CancellationNotice {
        TestWF::UI    ui;
        atomic<bool>& observed;
        bool          cancelled{true};
        ~CancellationNotice() {
            if (!cancelled)
                return;
            observed = true;
            ui.jsFunction("webfrontTests.senderWasCancelled")();
        }
    };

    Task awaitCancelledSender(TestWF::UI ui) {
        CancellationNotice notice{ui, state.senderCancellationObserved};
        try {
            co_await ui.jsFunctionSender<string>("webfrontTests.senderSlow")();
            notice.cancelled = false;
        } catch (...) {
            notice.cancelled = false;
            throw;
        }
        throw runtime_error("Cancelled browser sender resumed with a result");
    }

    void registerBitsetCallbacks() {
        webFront.cppFunction("bitsetEcho", [](const bitset<9>& value) {
            return value;
        });
        webFront.cppFunction("bitsetEmpty", [](bitset<0> value) {
            return value;
        });
        webFront.cppFunction("bitsetGrid", [](const bitset<1024>& value) {
            return value;
        });
        webFront.cppFunction("bitsetTuple", [](tuple<bitset<9>, int> value) {
            return value;
        });
    }

    void registerCallbacks() {
        webFront.cppFunction("cancelBrowserSender", [this] {
            if (cancelledTask)
                cancelledTask->requestStop();
        });
        webFront.cppFunction("senderCancellationObserved", [this] {
            return state.senderCancellationObserved.load();
        });
        webFront.cppFunction("plusPageUrl", [] { return frontend::detail::applicationUrl("9002", "a+b.html"); });
        webFront.cppFunction("signal.generate", &demo::generateSignal);
        webFront.cppFunction<string>("rejectedWebSocketUrl", [this] { return rejectedWebSocket.url(); });
        uiSubscription = webFront.subscribeUIStarted([this](TestWF::UI) {
            state.subscriberNotified = true;
        });
        webFront.cppFunction("callBackCaller", [](TestWF::UI caller, const string& token) {
            caller.jsFunction("webfrontTests.receiveFromCaller")(token);
        });
        webFront.cppFunction("browserReady", [this](TestWF::UI ui) {
            // Raw origin probes create extra sessions; bind calls to the document reporting ready.
            connectedUI.emplace(ui);
            browserReady();
        });
        webFront.cppFunction<void, string>("recordFromJs", [this](const string& token) {
            recordFromJs(token);
        });
        webFront.cppFunction<void, string>("recordExtendedFrameFromJs", [this](const string& value) {
            state.jsExtendedFrameObserved = value == string(126, 'x');
        });
        webFront.cppFunction<void, string, string>("reportJasmine", [this](const string& overallStatus, const string& failures) {
            reportJasmine(overallStatus, failures);
        });
        webFront.cppFunction<string, string>("returnFromCpp", [](const string& value) { return "cpp-result:" + value; });
        webFront.cppFunction<void>("throwFromCpp", [] { throw runtime_error("C++ callback failed"); });
        webFront.cppFunction<string>("returnOverlongFromCpp", [] { return string(65536, 'x'); });
        webFront.cppFunction("integerInCpp", [](int value) {
            return value;
        });
        webFront.cppFunction<size_t, string>("lengthInCpp", [](const string& value) { return value.size(); });
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
                recordArraysFromJs(u8, i8, u16, i16, u32, i32, u64, i64, floats, doubles);
            });
        webFront.cppFunction<void, tuple<int, string>>("recordTupleFromJs", [this](const tuple<int, string>& value) {
            state.jsTupleObserved = value == tuple<int, string>{42, "tuple"};
        });
    }

    /// Commands driven by webtest/js/terminal-specs.mjs, and the tool manager driven by
    /// webtest/js/tools-specs.mjs against the fake registry under webtest/registry/.
    void registerTerminalCommands() {
        Terminal terminal{webFront, {.prompt = "test> ", .banner = "webtest terminal"}};
        terminal.command("greet", "Greet someone", [](CommandArgs args) {
            return "Hello, " + (args.empty() ? string{"world"} : args[0]) + "!";
        });
        terminal.command("fail", "Throw from C++", [](CommandArgs) -> string {
            throw runtime_error("terminal command failed");
        });
        ToolManager<TestWF, TestToolsProject> tools{webFront, terminal, {.registry = "/registry/"}};
    }

    /// The JS side echoes back the very arrays browserReady() sent it, so the cpp* members double as the
    /// expected values and there is a single place to edit when the fixtures change.
    template <typename T, size_t N>
    static bool matches(const vector<T>& actual, const array<T, N>& expected) {
        return equal(actual.begin(), actual.end(), expected.begin(), expected.end());
    }

    /// Compares two same-arity tuples of references element-wise.
    template <typename Actual, typename Expected>
    static bool matchesAll(const Actual& actual, const Expected& expected) {
        static_assert(tuple_size_v<Actual> == tuple_size_v<Expected>, "Echoed arrays and fixtures must correspond");
        return [&]<size_t... I>(index_sequence<I...>) {
            return (matches(get<I>(actual), get<I>(expected)) && ...);
        }(make_index_sequence<tuple_size_v<Actual>>{});
    }

    /// Arity follows the bridge signature under test: ten typed arrays as ten top-level parameters.
    template <typename... Arrays>
    void recordArraysFromJs(const Arrays&... arrays) {
        state.jsArraysObserved =
          matchesAll(tie(arrays...), tie(cppU8, cppI8, cppU16, cppI16, cppU32, cppI32, cppU64, cppI64, cppFloat, cppDouble));
    }

    void browserReady() {
        state.browserReady = true;
        webFront.spawn(checkSenderResults(requireUI()));
        startBitsetFutureChecks(requireUI());
        webFront.spawn(checkBitsetResults(requireUI()));
        cancelledTask = requireUI().spawn(awaitCancelledSender(requireUI()));
        requireUI().jsFunction("webfrontTests.receiveFromCpp")(cppToJsToken);
        requireUI().jsFunction("webfrontTests.receiveArraysFromCpp")(
          cppU8, cppI8, cppU16, cppI16, cppU32, cppI32, cppU64, cppI64, cppFloat, cppDouble);
        cppResult = requireUI().template jsFunction<string>("webfrontTests.returnToCpp")("from-cpp");
    }

    void recordFromJs(const string& token) {
        state.jsToCppObserved = token == jsToCppToken;
    }

    bool cppResultMatches() {
        if (!cppResult.valid()) return false;
        try {
            if (cppResult.wait_for(cppResultWaitTimeout) != future_status::ready) {
                log::error("C++ result call timed out after {} seconds", cppResultWaitTimeout.count());
                return false;
            }
            return cppResult.get() == "js-result:from-cpp";
        } catch (const exception& error) {
            log::error("C++ result call failed: {}", error.what());
            return false;
        }
    }

    void reportJasmine(const string& overallStatus, const string& failures) {
        state.jasmineReported   = true;
        state.cppResultObserved = cppResultMatches();
        state.passed            = overallStatus == "passed" && state.browserReady && state.subscriberNotified && browserCallsObserved() && cppCallsObserved()
                                  && rejectedWebSocket.rejectedRequests() >= 2;
        if (!failures.empty())
            log::error("Jasmine failures:\n{}", failures);
        requireUI().jsFunction("webfrontTests.close")(state.passed.load());
    }

    bool cppCallsObserved() const {
        return state.cppResultObserved && state.senderResultsObserved && state.senderCancellationObserved;
    }

    bool browserCallsObserved() const {
        return state.jsToCppObserved && state.jsExtendedFrameObserved && state.jsArraysObserved && state.jsTupleObserved;
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
    const auto consoleSink = log::addSinks(log::clogSink);

    int result = 1;
    try {
        result = runBrowserIntegration();
    } catch (const exception& error) {
        log::error("Browser integration test failed: {}", error.what());
        cerr << error.what() << '\n';
    }
    error_code ignored;
    filesystem::remove_all(TestToolsProject::get(), ignored);
    log::removeSinks(consoleSink);
    return result;
}
