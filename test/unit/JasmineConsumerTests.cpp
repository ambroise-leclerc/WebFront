#include <webfront/testing/Bdd.hpp>
#include <webfront/testing/Capture.hpp>
#include <webfront/testing/Jasmine.hpp>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace webfront::bdd;

const Scenario consumerJasmine{"Consumer Jasmine configuration is bounded and rejects ambiguous or incomplete input", [] {
    using webfront::testing::detail::jasmineConfiguration;
    given("a runtime, a spec and an explicit report destination", [&] {
        const auto config = jasmineConfiguration(R"({"files":["jasmine.js","board.spec.js"],"report":"report.json"})");
        require(config.files == std::vector<std::string>{"jasmine.js", "board.spec.js"});
        require(config.report == "report.json");
    });
    given("an invalid or ambiguous configuration", [&] {
        for (const auto text : {R"({"files":["jasmine.js"],"report":"r"})",
                                R"({"files":["j","s"]})",
                                R"({"files":["j","s"],"report":"r","report":"other"})",
                                R"({"files":["j","s"],"report":"r","shell":true})",
                                "{} trailing"})
            requireThrows([&] { jasmineConfiguration(text); });
    });
}};

namespace {
using webfront::testing::detail::JasmineSession;

struct ReportFile {
    // One file per scenario: CTest runs scenarios as parallel processes.
    std::filesystem::path path;
    explicit ReportFile(const std::string& name) : path(std::filesystem::temp_directory_path() / ("webfront-jasmine-" + name + ".json")) {
        std::filesystem::remove(path);
    }
    ~ReportFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::string text() const {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};
}  // namespace

const Scenario jasmineSession{"A Jasmine session serves its files in bounded chunks to the instrumented page only", [] {
    static constexpr auto chunk = JasmineSession<>::chunkSize;
    ReportFile     report{"chunks"};
    given("a file that is an exact multiple of the chunk size, an empty file and a short one", [&] {
        JasmineSession session{{std::string(chunk * 2, 'a'), "", "short"}, report.path};
        require(session.attach(7));
        require(session.count(7) == 3);
        then("every chunk is full until an empty read ends the file", [&] {
            require(session.read(7, 0, 0).size() == chunk);
            require(session.read(7, 0, static_cast<double>(chunk)).size() == chunk);
            require(session.read(7, 0, static_cast<double>(chunk * 2)).empty());
            require(session.read(7, 1, 0).empty());
            require(session.read(7, 2, 2).size() == 3);
        });
        then("indexes and offsets outside the file are rejected", [&] {
            for (const auto& [index, offset] : {std::pair{-1.0, 0.0}, {3.0, 0.0}, {0.5, 0.0}, {0.0, -1.0}, {0.0, 0.5},
                                               {0.0, static_cast<double>(chunk * 2 + 1)}, {std::nan(""), 0.0}, {0.0, std::nan("")},
                                               {0.0, HUGE_VAL}})
                requireThrows([&] { (void)session.read(7, index, offset); });
        });
        then("another page cannot read, count or report", [&] {
            requireThrows([&] { (void)session.count(8); });
            requireThrows([&] { (void)session.read(8, 0, 0); });
            requireThrows([&] { session.result(8, R"({"status":"passed"})"); });
        });
    });
    given("a page that was never attached", [&] {
        JasmineSession session{{"x"}, report.path};
        requireThrows([&] { (void)session.count(1); });
    });
}};

const Scenario reloadedPage{"A reloaded page replaces the previous one, and nothing attaches after completion", [] {
    ReportFile     report{"reload"};
    JasmineSession session{{"x"}, report.path};
    require(session.attach(1));
    require(session.attach(2));
    requireThrows([&] { (void)session.count(1); });
    require(session.count(2) == 1);
    session.result(2, R"({"status":"passed"})");
    require(!session.attach(3));
}};

const Scenario jasmineSessionValidates{"A Jasmine session validates and records exactly one result", [] {
    ReportFile report{"result"};
    given("an attached page", [&] {
        JasmineSession session{{"x"}, report.path};
        require(session.exitCode() == 1);
        require(session.attach(1));
        then("a malformed or ambiguous result is rejected and leaves the run failed", [&] {
            for (const auto text : {"", "{}", R"({"status":"ok"})", R"({"status":"passed","status":"passed"})", "[]", R"({"status":"passed"} x)"})
                requireThrows([&] { session.result(1, text); });
            require(session.exitCode() == 1);
            require(!std::filesystem::exists(report.path));
        });
        then("a passing result is written once and a second one is refused", [&] {
            session.result(1, R"({"format":1,"status":"passed","total":1})");
            require(session.exitCode() == 0);
            require(report.text() == "{\"format\":1,\"status\":\"passed\",\"total\":1}\n");
            requireThrows([&] { session.result(1, R"({"status":"failed"})"); });
            require(session.exitCode() == 0);
        });
        then("failed and error results keep a nonzero status", [&] {
            session.result(1, R"({"status":"error","error":"boom"})");
            require(session.exitCode() == 1);
        });
    });
    given("a report that cannot be written", [&] {
        JasmineSession session{{"x"}, std::filesystem::temp_directory_path() / "webfront-missing-dir" / "report.json"};
        require(session.attach(1));
        requireThrows([&] { session.result(1, R"({"status":"passed"})"); });
        require(session.exitCode() == 1);
    });
}};

const Scenario injectedScript{"The injected script reads in the same chunk size the session serves and reports load errors", [] {
    const auto script = webfront::testing::detail::jasmineScript();
    require(script.starts_with("const CHUNK = " + std::to_string(JasmineSession<>::chunkSize) + ";"));
    require(script.find("49152", script.find("49152") + 1) == std::string::npos);  // the number appears only in the CHUNK definition
    require(script.find("failed to load") != std::string_view::npos);
}};


namespace {
using webfront::frontend::CaptureResult;
using webfront::testing::CaptureStore;

struct CaptureDirectory {
    std::filesystem::path path;
    explicit CaptureDirectory(const std::string& name) : path(std::filesystem::temp_directory_path() / ("webfront-capture-" + name)) {
        std::filesystem::remove_all(path);
    }
    ~CaptureDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

CaptureResult fakeImage(std::chrono::milliseconds) {
    return std::vector<std::uint8_t>{0x89, 'P', 'N', 'G'};
}

bool fileExists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}
}  // namespace

const Scenario captureStoreScenario{"A capture store writes images only below its directory", [] {
    CaptureDirectory directory{"names"};
    given("a store with a window that answers", [&] {
        CaptureStore store{directory.path, fakeImage};
        then("a valid name becomes a PNG file", [&] {
            require(store.screenshot("board-1_a") == 4);
            require(std::filesystem::file_size(directory.path / "board-1_a.png") == 4);
        });
        then("a name that could leave the directory or add an extension is refused", [&] {
            for (const auto* name : {"", "../x", "a/b", "a\\b", "a.png", ".", "..", "a b", "caf\xC3\xA9", "\xE9"})
                requireThrows<std::invalid_argument>([&] { store.screenshot(name); });
            requireThrows<std::invalid_argument>([&] { store.screenshot(std::string(65, 'a')); });
            require(!std::filesystem::exists(directory.path));
        });
    });
    given("a window that cannot be captured", [&] {
        CaptureStore store{directory.path, [](std::chrono::milliseconds) -> CaptureResult {
                               return std::unexpected("no window");
                           }};
        then("the diagnostic reaches the caller and nothing is written", [&] {
            requireThrows([&] { store.screenshot("x"); }, equals("Capture failed: no window"));
            require(!std::filesystem::exists(directory.path / "x.png"));
        });
    });
}};

const Scenario captureStoreRecords{"A capture store records numbered images at a bounded rate", [] {
    CaptureDirectory directory{"record"};
    std::atomic<int> calls = 0;
    CaptureStore     store{directory.path, [&](std::chrono::milliseconds) -> CaptureResult {
                           ++calls;
                           return std::vector<std::uint8_t>{1, 2, 3};
                           }};
    given("a recording at 20 images per second", [&] {
        store.startRecording("run", 20);
        then("a second recording is refused while it runs", [&] {
            requireThrows<std::logic_error>([&] { store.startRecording("other", 10); });
        });
        requireEventually([&] {
            return calls >= 4;
        });
        const auto frames = store.stopRecording();
        then("the images are numbered from 0001 and roughly match the rate", [&] {
            require(frames >= 4);
            require(frames <= 12);
            require(std::filesystem::exists(directory.path / "run" / "0001.png"));
            require(std::filesystem::exists(directory.path / "run" / "0004.png"));
            require(frames == static_cast<std::size_t>(calls.load()));
        });
        then("stopping again is an error", [&] {
            requireThrows<std::logic_error>([&] { store.stopRecording(); });
        });
    });
    given("a rate outside 1 to 30 or an invalid name", [&] {
        requireThrows<std::invalid_argument>([&] { store.startRecording("run", 0); });
        requireThrows<std::invalid_argument>([&] { store.startRecording("run", 31); });
        requireThrows<std::invalid_argument>([&] { store.startRecording("../run", 10); });
    });
}};

const Scenario recordingLoses{"A recording that loses its window reports the failure when it is stopped", [] {
    CaptureDirectory directory{"failure"};
    std::atomic<int> calls = 0;
    CaptureStore     store{directory.path, [&](std::chrono::milliseconds) -> CaptureResult {
                           if (++calls > 2)
                               return std::unexpected("the window is closed");
                           return std::vector<std::uint8_t>{1};
                           }};
    store.startRecording("lost", 30);
    requireEventually([&] {
        return calls > 2;
    });
    requireThrows([&] { store.stopRecording(); }, equals("Recording stopped: Capture failed: the window is closed"));
}};

namespace {
template <typename Poll>
double pollUntilDone(Poll poll, std::source_location location = std::source_location::current()) {
    double value = -1;
    requireEventually(
        [&] {
            return (value = poll()) >= 0;
        },
        std::chrono::seconds{30},
        location);
    return value;
}
}  // namespace

const Scenario captureStoreTakes{"A capture store takes a screenshot without making the caller wait for the window", [] {
    CaptureDirectory  directory{"async"};
    std::atomic<bool> release = false;
    CaptureStore      store{directory.path, [&](std::chrono::milliseconds) -> CaptureResult {
                           while (!release)
                               std::this_thread::sleep_for(std::chrono::milliseconds{1});
                           return std::vector<std::uint8_t>{1, 2, 3};
                            }};
    given("a window that has not answered yet", [&] {
        store.beginScreenshot("slow");  // returns at once
        then("it is pending, a second one is refused, and the result arrives later", [&] {
            require(!store.pollScreenshot().has_value());
            requireThrows<std::logic_error>([&] { store.beginScreenshot("other"); });
            release = true;
            std::optional<std::size_t> size;
            requireEventually([&] { return (size = store.pollScreenshot()).has_value(); });
            require(size == 3);
            require(std::filesystem::exists(directory.path / "slow.png"));
            requireThrows<std::logic_error>([&] { store.pollScreenshot(); });
        });
    });
    given("an invalid name", [&] {
        requireThrows<std::invalid_argument>([&] { store.beginScreenshot("../x"); });
    });
    release = true;
}};

const Scenario asynchronousScreenshot{"An asynchronous screenshot reports a capture failure when polled", [] {
    CaptureDirectory directory{"asyncfail"};
    CaptureStore     store{directory.path, [](std::chrono::milliseconds) -> CaptureResult {
                           return std::unexpected("no window");
                           }};
    store.beginScreenshot("x");
    std::optional<std::size_t> size;
    requireThrows(
        [&] {
            [&] {
                waitUntil([&] {
                    return (size = store.pollScreenshot()).has_value();
                });
            }();
        },
        equals("Capture failed: no window"));
}};

const Scenario recordingStops{"A recording stops on request and reports its count by polling", [] {
    CaptureDirectory directory{"stop"};
    CaptureStore     store{directory.path, fakeImage};
    store.startRecording("run", 30);
    requireEventually([&] {
        return fileExists(directory.path / "run" / "0002.png");
    });
    store.requestStop();
    std::optional<std::size_t> frames;
    requireEventually([&] { return (frames = store.pollRecording()).has_value(); });
    require(frames.has_value());
    require(*frames >= 2);
    requireThrows<std::logic_error>([&] { store.requestStop(); });
    requireThrows<std::logic_error>([&] { store.pollRecording(); });
}};

const Scenario recordingEnds{"A recording ends by itself at the image limit and can still be collected", [] {
    CaptureDirectory directory{"limit"};
    CaptureStore     store{directory.path, fakeImage, 3};
    store.startRecording("run", 30);
    requireEventually([&] {
        return fileExists(directory.path / "run" / "0003.png");
    });
    store.requestStop();  // finished or about to: not an error
    std::optional<std::size_t> frames;
    requireEventually([&] { return (frames = store.pollRecording()).has_value(); });
    require(frames == 3);
    require(!std::filesystem::exists(directory.path / "run" / "0004.png"));
    store.startRecording("again", 30);  // the slot is free once collected
    require(store.stopRecording() <= 3);
}};

const Scenario captureNobody{"A capture that nobody collects does not block the next one", [] {
    CaptureDirectory directory{"uncollected"};
    CaptureStore     store{directory.path, fakeImage, 2};
    given("a screenshot that finished but was never polled", [&] {
        store.beginScreenshot("a");
        then("a concurrent request cannot take over its result", [&] {
            requireEventually([&] { return fileExists(directory.path / "a.png"); });
            requireThrows<std::logic_error>([&] { store.beginScreenshot("b"); });
            require(pollUntilDone([&] {
                        const auto size = store.pollScreenshot();
                        return size ? static_cast<double>(*size) : -1.0;
                    }) == 4);
            require(std::filesystem::exists(directory.path / "a.png"));
            require(!std::filesystem::exists(directory.path / "b.png"));
        });
    });
    given("a screenshot that finished but whose page was replaced", [&] {
        store.beginScreenshot("a");
        then("abandon frees the slot and the next screenshot starts and can be polled", [&] {
            requireEventually([&] {  // abandon frees the slot as soon as the first worker has ended
                store.abandon();
                try {
                    store.beginScreenshot("b");
                    return true;
                } catch (const std::logic_error&) {
                    return false;
                }
            });
            require(pollUntilDone([&] {
                        const auto size = store.pollScreenshot();
                        return size ? static_cast<double>(*size) : -1.0;
                    }) == 4);
            require(std::filesystem::exists(directory.path / "b.png"));
        });
    });
    given("a recording that ended by itself and was never collected", [&] {
        store.startRecording("first", 30);
        then("another recording starts, and the first one keeps its images", [&] {
            requireEventually([&] {  // the slot is free as soon as the first worker has ended
                try {
                    store.startRecording("second", 30);
                    return true;
                } catch (const std::logic_error&) {
                    return false;
                }
            });
            require(store.stopRecording() <= 2);
            require(std::filesystem::exists(directory.path / "first" / "0002.png"));
        });
    });
}};

const Scenario recordingNever{"A recording never erases the images of an earlier recording of the same run", [] {
    CaptureDirectory directory{"duplicate"};
    CaptureStore     store{directory.path, fakeImage};
    store.startRecording("run", 30);
    requireEventually([&] {
        return fileExists(directory.path / "run" / "0001.png");
    });
    const auto frames = store.stopRecording();
    require(frames >= 1);
    requireThrows<std::logic_error>([&] { store.startRecording("run", 30); });
    require(std::filesystem::exists(directory.path / "run" / "0001.png"));
    given("images left by an earlier run in a fresh store", [&] {
        CaptureStore fresh{directory.path, fakeImage};
        then("the first recording of that name replaces them", [&] {
            std::ofstream{directory.path / "run" / "stale.png"} << "x";
            fresh.startRecording("run", 30);
            fresh.stopRecording();
            require(!std::filesystem::exists(directory.path / "run" / "stale.png"));
        });
    });
}};

const Scenario stopRequest{"A stop request belongs to its own recording", [] {
    CaptureDirectory directory{"restart"};
    CaptureStore     store{directory.path, fakeImage, 3};
    store.startRecording("one", 30);
    store.requestStop();
    std::optional<std::size_t> frames;
    requireEventually([&] { return (frames = store.pollRecording()).has_value(); });
    store.startRecording("two", 30);  // must not inherit, nor lose, the earlier stop request
    requireEventually([&] {
        return fileExists(directory.path / "two" / "0003.png");
    });
    require(store.stopRecording() == 3);  // reached its own image limit: it was not stopped early by the first request
}};

namespace {
/// State shared with a source that a detached worker may still run after the test ended: never a reference to a local.
struct Gate {
    std::atomic<bool> release = false, entered = false, left = false;
};

CaptureStore::Source blockedUntilReleased(std::shared_ptr<Gate> gate) {
    return [gate](std::chrono::milliseconds) -> CaptureResult {
        gate->entered = true;
        while (!gate->release)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        gate->left = true;
        return std::vector<std::uint8_t>{1};
    };
}
}  // namespace

const Scenario destroyingStore{"Destroying a store does not wait for a window that never answers", [] {
    CaptureDirectory directory{"closing"};
    const auto       gate    = std::make_shared<Gate>();
    const auto       started = std::chrono::steady_clock::now();
    {
        CaptureStore store{directory.path, blockedUntilReleased(gate)};
        store.beginScreenshot("stuck");
        requireEventually([&] {
            return gate->entered.load();
        });
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    gate->release      = true;  // the detached worker ends alone, only touching state it owns
    requireEventually([&] {
        return gate->left.load();
    });
    require(elapsed < std::chrono::seconds{5});
}};

const Scenario captureCallbacks{"Capture callbacks of a Jasmine session obey the same page authority", [] {
    CaptureDirectory directory{"session"};
    ReportFile       report{"capture"};
    auto             store = std::make_shared<CaptureStore>(directory.path, fakeImage);
    JasmineSession   session{{"x"}, report.path, store};
    require(session.attach(1));
    then("the instrumented page captures and another page cannot", [&] {
        session.screenshotStart(1, "ok");
        require(pollUntilDone([&] {
                    return session.screenshotPoll(1);
                })
                == 4);
        requireThrows([&] { session.screenshotStart(2, "no"); });
        requireThrows([&] { (void)session.screenshotPoll(2); });
        requireThrows([&] { session.recordStart(2, "no", 10); });
        requireThrows([&] { session.recordStop(2); });
        requireThrows([&] { (void)session.recordPoll(2); });
    });
    then("a recording is stopped and collected through the session", [&] {
        session.recordStart(1, "clip", 30);
        requireEventually([&] {
            return fileExists(directory.path / "clip" / "0002.png");
        });
        session.recordStop(1);
        require(pollUntilDone([&] {
                    return session.recordPoll(1);
                })
                >= 2);
    });
    then("a page that replaces the instrumented one finds the recording slot free", [&] {
        session.recordStart(1, "abandoned", 30);
        require(session.attach(2));  // a reload: page 1 can no longer collect its recording
        // The slot is free as soon as attach returns, with no polling for the old recording to end.
        requireNothrow([&] { session.recordStart(2, "again", 30); });
        session.recordStop(2);
    });
    then("a session without a directory refuses to capture", [&] {
        JasmineSession bare{{"x"}, report.path};
        require(bare.attach(1));
        requireThrows([&] { bare.screenshotStart(1, "ok"); });
    });
}};

const Scenario windowRegistration{"A window registration is replaced by a later one and never removes its successor", [] {
    using webfront::frontend::capturePng;
    using webfront::frontend::CaptureRegistration;
    const auto answer = [](std::uint8_t byte) {
        return [byte](std::chrono::milliseconds) -> CaptureResult {
            return std::vector<std::uint8_t>{byte};
        };
    };
    require(!capturePng().has_value());  // nothing registered
    {
        CaptureRegistration first{answer(1)};
        require(capturePng().value() == std::vector<std::uint8_t>{1});
        {
            CaptureRegistration second{answer(2)};
            require(capturePng().value() == std::vector<std::uint8_t>{2});
            first = CaptureRegistration{};  // the stale one goes away
            require(capturePng().value() == std::vector<std::uint8_t>{2});
            CaptureRegistration moved{std::move(second)};
            require(capturePng().value() == std::vector<std::uint8_t>{2});
        }
        require(!capturePng().has_value());  // the last owner closed the window
    }
}};

const Scenario jasmineConfigurationAccepts{"The Jasmine configuration accepts an optional capture directory", [] {
    using webfront::testing::detail::jasmineConfiguration;
    require(jasmineConfiguration(R"({"files":["j","s"],"report":"r","captures":"c"})").captures == "c");
    require(jasmineConfiguration(R"({"files":["j","s"],"report":"r"})").captures.empty());
    requireThrows([&] { jasmineConfiguration(R"({"files":["j","s"],"report":"r","captures":"a","captures":"b"})"); });
}};

const Scenario abandoningRecording{"Abandoning a recording frees its slot before it returns, unless the window blocks the worker", [] {
    CaptureDirectory directory{"abandon"};
    given("a window that answers", [&] {
        CaptureStore store{directory.path, fakeImage};
        store.startRecording("first", 30);
        store.abandon();
        then("a new recording starts at once", [&] {
            requireNothrow([&] { store.startRecording("second", 30); });
            requireNothrow([&] { store.stopRecording(); });
        });
        then("abandoning with nothing running is not an error", [&] {
            store.abandon();
            requireNothrow([&] { store.abandon(); });
        });
    });
    given("a window that does not answer", [&] {
        const auto gate = std::make_shared<Gate>();
        {
            CaptureStore store{directory.path, blockedUntilReleased(gate)};
            store.startRecording("blocked", 30);
            requireEventually([&] {
                return gate->entered.load();
            });  // The worker is inside the window's capture, not yet started.
            const auto begin = std::chrono::steady_clock::now();
            store.abandon();
            then("it waits only the grace period, and the slot stays taken", [&] {
                require(std::chrono::steady_clock::now() - begin < std::chrono::seconds{3});
                requireThrows<std::logic_error>([&] { store.startRecording("next", 30); });
            });
            gate->release = true;
        }
        requireEventually([&] {
            return gate->left.load();
        });
    });
}};

const Scenario twoRecording{"Two recording names that differ only by case are one recording, as on a case-insensitive file system", [] {
    CaptureDirectory directory{"case"};
    CaptureStore     store{directory.path, fakeImage};
    store.startRecording("run", 30);
    requireEventually([&] {
        return fileExists(directory.path / "run" / "0002.png");
    });
    const auto frames = store.stopRecording();
    then("the second name is refused before anything is erased", [&] {
        requireThrows<std::logic_error>([&] { store.startRecording("Run", 30); });
        requireThrows<std::logic_error>([&] { store.startRecording("RUN", 30); });
        require(fileExists(directory.path / "run" / "0001.png"));
        require(fileExists(directory.path / "run" / "0002.png"));
        require(frames >= 2);
    });
    then("a different name is still accepted", [&] {
        requireNothrow([&] { store.startRecording("run2", 30); });
        store.stopRecording();
    });
}};

namespace {
struct FakeCapture { };
}  // namespace

const Scenario windowCloses{"A window that closes answers the captures it never got to run", [] {
    using Pending = webfront::frontend::PendingCaptures<FakeCapture>;
    given("a request queued for a window whose event loop will never serve it", [&] {
        Pending pending;
        const auto request = std::make_shared<FakeCapture>();
        pending.open();
        require(pending.track(request));
        when("the window closes", [&] {
            const auto unanswered = pending.close();
            then("the request is handed back to be answered at once, not left to the capture timeout", [&] {
                require(unanswered.size() == 1);
                require(unanswered.front() == request);
            });
            then("a request made after the closing is refused instead of queued", [&] {
                require(!pending.track(std::make_shared<FakeCapture>()));
                require(pending.close().empty());
            });
        });
        when("the request is answered or gives up before the window closes", [&] {
            pending.untrack(request);
            then("closing has nothing left to answer", [&] {
                require(pending.close().empty());
            });
        });
    });
    given("a window that has not opened yet", [&] {
        Pending pending;
        then("no request is accepted", [&] {
            require(!pending.track(std::make_shared<FakeCapture>()));
        });
    });
    given("a window that is closed and then opened again", [&] {
        Pending pending;
        pending.open();
        static_cast<void>(pending.close());
        pending.open();
        then("requests are accepted again", [&] {
            require(pending.track(std::make_shared<FakeCapture>()));
        });
    });
}};
