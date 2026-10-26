// The browser integration test: the specs of test/web/suite run under `wf test` in the developer environment of an
// application that registers the C++ callbacks they call. Usage is that of webfront-wf (test/driver/WfDriver.hpp), e.g.
//   webtest --project <copy of test/web/suite, after wf install> --registry-dir <registry> -- test --timeout 120
// The status is that of `wf test`, and 1 when the specs passed but C++ did not observe every call they make.
#include <webfront/system/FileSystem.hpp>
#include <webfront/tooling/Logger.hpp>
#include <webfront/WebFront.hpp>

#include "../driver/WfDriver.hpp"
#include "RejectedWebSocketServer.hpp"

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
    atomic<bool> specsFinished{false};
    atomic<bool> subscriberNotified{false};
};

// The pages the specs fetch or embed (origin probes, a path containing a plus), served from the project directory.
class TestPages {
public:
    explicit TestPages(const filesystem::path& project) : root(project / "pages") { }

    optional<fs::File> open(filesystem::path file) const {
        try {
            const auto path = developer::projectPath(root, file.relative_path().generic_string());
            ifstream   stream(path, ios::binary);
            if (stream && filesystem::is_regular_file(path))
                return fs::File{std::move(stream)};
        } catch (const exception&) {}
        return {};
    }

private:
    filesystem::path root;
};

// CMake chooses an embedded frontend; the same specs run under CEF/Xvfb, WebKitGTK, WebView2 and WKWebView.
#ifndef WEBFRONT_TEST_FRONTEND
    #define WEBFRONT_TEST_FRONTEND webfront::frontend::CEFFrontend
#endif
using TestWF = BasicDeveloperApp<testdriver::DriverFrontend<WEBFRONT_TEST_FRONTEND>, testdriver::DriverFS, TestPages>;

class BrowserIntegrationTest {
public:
    using Pages = TestPages;

    ~BrowserIntegrationTest() {
        joinBitsetFutureChecks();
    }

    /// Registers the callbacks of the specs before the page opens; the rejecting WebSocket server starts after the CEF
    /// subprocesses have dispatched (driver main), in the browser process.
    void configure(TestWF& app) {
        log::info("Starting automated Jasmine browser integration test");
        webFront = &app;
        rejectedWebSocket.emplace();
        registerCallbacks();
        registerBitsetCallbacks();
    }

    /// The status of `wf test` (the specs), unless the specs passed and C++ did not observe every call they make.
    int finish(int status) {
        joinBitsetFutureChecks();
        if (status != 0) {
            log::error("wf test did not pass (status {})", status);
            return status;
        }
        return result();
    }

private:
    TestWF*                          webFront{};
    optional<RejectedWebSocketServer> rejectedWebSocket;
    optional<TaskHandle>             cancelledTask;
    TestState            state;
    optional<TestWF::UI> connectedUI;
    UISubscription                   uiSubscription;
    future<string> cppResult;
    jthread bitsetFutureThread;
    static constexpr chrono::seconds cppResultWaitTimeout{10};

    void joinBitsetFutureChecks() {
        if (bitsetFutureThread.joinable())
            bitsetFutureThread.join();
    }

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
        webFront->spawn(reportBitsetFutureChecks(ui, passed));
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
        webFront->cppFunction("bitsetEcho", [](const bitset<9>& value) {
            return value;
        });
        webFront->cppFunction("bitsetEmpty", [](bitset<0> value) {
            return value;
        });
        webFront->cppFunction("bitsetGrid", [](const bitset<1024>& value) {
            return value;
        });
        webFront->cppFunction("bitsetTuple", [](tuple<bitset<9>, int> value) {
            return value;
        });
    }

    void registerCallbacks() {
        webFront->cppFunction("cancelBrowserSender", [this] {
            if (cancelledTask)
                cancelledTask->requestStop();
        });
        webFront->cppFunction("senderCancellationObserved", [this] {
            return state.senderCancellationObserved.load();
        });
        webFront->cppFunction("plusPageUrl", [this] { return frontend::detail::applicationUrl(webFront->port(), "a+b.html"); });
        webFront->cppFunction<string>("rejectedWebSocketUrl", [this] { return rejectedWebSocket->url(); });
        uiSubscription = webFront->subscribeUIStarted([this](TestWF::UI) {
            state.subscriberNotified = true;
        });
        webFront->cppFunction("callBackCaller", [](TestWF::UI caller, const string& token) {
            caller.jsFunction("webfrontTests.receiveFromCaller")(token);
            return "cpp-result:" + token;
        });
        webFront->cppFunction("browserReady", [this](TestWF::UI ui) {
            // Raw origin probes create extra sessions; bind calls to the document reporting ready.
            connectedUI.emplace(ui);
            browserReady();
        });
        webFront->cppFunction<void, string>("recordFromJs", [this](const string& token) {
            recordFromJs(token);
        });
        webFront->cppFunction<void, string>("recordExtendedFrameFromJs", [this](const string& value) {
            state.jsExtendedFrameObserved = value == string(126, 'x');
        });
        webFront->cppFunction("specsFinished", [this] {
            specsFinished();
        });
        webFront->cppFunction<string, string>("returnFromCpp", [](const string& value) { return "cpp-result:" + value; });
        webFront->cppFunction<void>("throwFromCpp", [] { throw runtime_error("C++ callback failed"); });
        webFront->cppFunction<string>("returnOverlongFromCpp", [] { return string(65536, 'x'); });
        webFront->cppFunction("integerInCpp", [](int value) {
            return value;
        });
        webFront->cppFunction<size_t, string>("lengthInCpp", [](const string& value) { return value.size(); });
        webFront->cppFunction<void,
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
        webFront->cppFunction<void, tuple<int, string>>("recordTupleFromJs", [this](const tuple<int, string>& value) {
            state.jsTupleObserved = value == tuple<int, string>{42, "tuple"};
        });
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
        webFront->spawn(checkSenderResults(requireUI()));
        startBitsetFutureChecks(requireUI());
        webFront->spawn(checkBitsetResults(requireUI()));
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

    /// The last spec: everything the specs sent over this connection has been handled.
    void specsFinished() {
        state.cppResultObserved = cppResultMatches();
        state.specsFinished     = true;
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
        if (!state.specsFinished) {
            log::error("The specs did not report the end of the run");
            return 1;
        }
        if (!(state.browserReady && state.subscriberNotified && browserCallsObserved() && cppCallsObserved()
              && rejectedWebSocket->rejectedRequests() >= 2)) {
            log::error("The specs passed but one of the bridge-direction checks failed");
            return 1;
        }

        log::info("Automated Jasmine browser integration test passed");
        return 0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    log::setLogLevel(log::Warn);
    const auto consoleSink = log::addSinks(log::clogSink);

    BrowserIntegrationTest test;
    const int              result = webfront::testdriver::main<WEBFRONT_TEST_FRONTEND>(argc, argv, "webtest", test);
    log::removeSinks(consoleSink);
    return result;
}
