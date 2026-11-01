#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <webfront/testing/Capture.hpp>
#include <webfront/testing/Jasmine.hpp>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

SCENARIO("Consumer Jasmine configuration is bounded and rejects ambiguous or incomplete input") {
    using webfront::testing::detail::jasmineConfiguration;
    GIVEN("a runtime, a spec and an explicit report destination") {
        const auto config = jasmineConfiguration(R"({"files":["jasmine.js","board.spec.js"],"report":"report.json"})");
        REQUIRE(config.files == std::vector<std::string>{"jasmine.js", "board.spec.js"});
        REQUIRE(config.report == "report.json");
    }
    GIVEN("an invalid or ambiguous configuration") {
        for (const auto text : {R"({"files":["jasmine.js"],"report":"r"})",
                                R"({"files":["j","s"]})",
                                R"({"files":["j","s"],"report":"r","report":"other"})",
                                R"({"files":["j","s"],"report":"r","shell":true})",
                                "{} trailing"})
            REQUIRE_THROWS(jasmineConfiguration(text));
    }
}

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

SCENARIO("A Jasmine session serves its files in bounded chunks to the instrumented page only") {
    constexpr auto chunk = JasmineSession<>::chunkSize;
    ReportFile     report{"chunks"};
    GIVEN("a file that is an exact multiple of the chunk size, an empty file and a short one") {
        JasmineSession session{
            {std::string(chunk * 2, 'a'), "", "short"},
            report.path
        };
        REQUIRE(session.attach(7));
        REQUIRE(session.count(7) == 3);
        THEN("every chunk is full until an empty read ends the file") {
            REQUIRE(session.read(7, 0, 0).size() == chunk);
            REQUIRE(session.read(7, 0, static_cast<double>(chunk)).size() == chunk);
            REQUIRE(session.read(7, 0, static_cast<double>(chunk * 2)).empty());
            REQUIRE(session.read(7, 1, 0).empty());
            REQUIRE(session.read(7, 2, 2).size() == 3);
        }
        THEN("indexes and offsets outside the file are rejected") {
            for (const auto& [index, offset] : {
                     std::pair{        -1.0,                                0.0},
                     {         3.0,                                0.0},
                     {         0.5,                                0.0},
                     {         0.0,                               -1.0},
                     {         0.0,                                0.5},
                     {         0.0, static_cast<double>(chunk * 2 + 1)},
                     {std::nan(""),                                0.0},
                     {         0.0,                       std::nan("")},
                     {         0.0,                           HUGE_VAL}
            })
                REQUIRE_THROWS(session.read(7, index, offset));
        }
        THEN("another page cannot read, count or report") {
            REQUIRE_THROWS(session.count(8));
            REQUIRE_THROWS(session.read(8, 0, 0));
            REQUIRE_THROWS(session.result(8, R"({"status":"passed"})"));
        }
    }
    GIVEN("a page that was never attached") {
        JasmineSession session{{"x"}, report.path};
        REQUIRE_THROWS(session.count(1));
    }
}

SCENARIO("A reloaded page replaces the previous one, and nothing attaches after completion") {
    ReportFile     report{"reload"};
    JasmineSession session{{"x"}, report.path};
    REQUIRE(session.attach(1));
    REQUIRE(session.attach(2));
    REQUIRE_THROWS(session.count(1));
    REQUIRE(session.count(2) == 1);
    session.result(2, R"({"status":"passed"})");
    REQUIRE_FALSE(session.attach(3));
}

SCENARIO("A Jasmine session validates and records exactly one result") {
    ReportFile report{"result"};
    GIVEN("an attached page") {
        JasmineSession session{{"x"}, report.path};
        REQUIRE(session.exitCode() == 1);
        REQUIRE(session.attach(1));
        THEN("a malformed or ambiguous result is rejected and leaves the run failed") {
            for (const auto text : {"", "{}", R"({"status":"ok"})", R"({"status":"passed","status":"passed"})", "[]", R"({"status":"passed"} x)"})
                REQUIRE_THROWS(session.result(1, text));
            REQUIRE(session.exitCode() == 1);
            REQUIRE_FALSE(std::filesystem::exists(report.path));
        }
        THEN("a passing result is written once and a second one is refused") {
            session.result(1, R"({"format":1,"status":"passed","total":1})");
            REQUIRE(session.exitCode() == 0);
            REQUIRE(report.text() == "{\"format\":1,\"status\":\"passed\",\"total\":1}\n");
            REQUIRE_THROWS(session.result(1, R"({"status":"failed"})"));
            REQUIRE(session.exitCode() == 0);
        }
        THEN("failed and error results keep a nonzero status") {
            session.result(1, R"({"status":"error","error":"boom"})");
            REQUIRE(session.exitCode() == 1);
        }
    }
    GIVEN("a report that cannot be written") {
        JasmineSession session{{"x"}, std::filesystem::temp_directory_path() / "webfront-missing-dir" / "report.json"};
        REQUIRE(session.attach(1));
        REQUIRE_THROWS(session.result(1, R"({"status":"passed"})"));
        REQUIRE(session.exitCode() == 1);
    }
}

SCENARIO("The injected script reads in the same chunk size the session serves and reports load errors") {
    const auto script = webfront::testing::detail::jasmineScript();
    REQUIRE(script.starts_with("const CHUNK = " + std::to_string(JasmineSession<>::chunkSize) + ";"));
    REQUIRE(script.find("49152", script.find("49152") + 1) == std::string::npos);  // the number appears only in the CHUNK definition
    REQUIRE(script.find("failed to load") != std::string_view::npos);
}


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
}  // namespace

SCENARIO("A capture store writes images only below its directory") {
    CaptureDirectory directory{"names"};
    GIVEN("a store with a window that answers") {
        CaptureStore store{directory.path, fakeImage};
        THEN("a valid name becomes a PNG file") {
            REQUIRE(store.screenshot("board-1_a") == 4);
            REQUIRE(std::filesystem::file_size(directory.path / "board-1_a.png") == 4);
        }
        THEN("a name that could leave the directory or add an extension is refused") {
            for (const auto* name : {"", "../x", "a/b", "a\\b", "a.png", ".", "..", "a b"})
                REQUIRE_THROWS_AS(store.screenshot(name), std::invalid_argument);
            REQUIRE_THROWS_AS(store.screenshot(std::string(65, 'a')), std::invalid_argument);
            REQUIRE_FALSE(std::filesystem::exists(directory.path));
        }
    }
    GIVEN("a window that cannot be captured") {
        CaptureStore store{directory.path, [](std::chrono::milliseconds) -> CaptureResult {
                               return std::unexpected("no window");
                           }};
        THEN("the diagnostic reaches the caller and nothing is written") {
            REQUIRE_THROWS_WITH(store.screenshot("x"), Catch::Matchers::Equals("Capture failed: no window"));
            REQUIRE_FALSE(std::filesystem::exists(directory.path / "x.png"));
        }
    }
}

SCENARIO("A capture store records numbered images at a bounded rate") {
    CaptureDirectory directory{"record"};
    std::atomic<int> calls = 0;
    CaptureStore     store{directory.path, [&](std::chrono::milliseconds) -> CaptureResult {
                           ++calls;
                           return std::vector<std::uint8_t>{1, 2, 3};
                           }};
    GIVEN("a recording at 20 images per second") {
        store.startRecording("run", 20);
        THEN("a second recording is refused while it runs") {
            REQUIRE_THROWS_AS(store.startRecording("other", 10), std::logic_error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{400});
        const auto frames = store.stopRecording();
        THEN("the images are numbered from 0001 and roughly match the rate") {
            REQUIRE(frames >= 4);
            REQUIRE(frames <= 12);
            REQUIRE(std::filesystem::exists(directory.path / "run" / "0001.png"));
            REQUIRE(std::filesystem::exists(directory.path / "run" / "0004.png"));
            REQUIRE(frames == static_cast<std::size_t>(calls.load()));
        }
        THEN("stopping again is an error") {
            REQUIRE_THROWS_AS(store.stopRecording(), std::logic_error);
        }
    }
    GIVEN("a rate outside 1 to 30 or an invalid name") {
        REQUIRE_THROWS_AS(store.startRecording("run", 0), std::invalid_argument);
        REQUIRE_THROWS_AS(store.startRecording("run", 31), std::invalid_argument);
        REQUIRE_THROWS_AS(store.startRecording("../run", 10), std::invalid_argument);
    }
}

SCENARIO("A recording that loses its window reports the failure when it is stopped") {
    CaptureDirectory directory{"failure"};
    std::atomic<int> calls = 0;
    CaptureStore     store{directory.path, [&](std::chrono::milliseconds) -> CaptureResult {
                           if (++calls > 2)
                               return std::unexpected("the window is closed");
                           return std::vector<std::uint8_t>{1};
                           }};
    store.startRecording("lost", 30);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    REQUIRE_THROWS_WITH(store.stopRecording(), Catch::Matchers::Equals("Recording stopped: Capture failed: the window is closed"));
}

namespace {
template <typename Poll>
double pollUntilDone(Poll poll) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (const auto value = poll(); value >= 0)
            return value;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return -1;
}
}  // namespace

SCENARIO("A capture store takes a screenshot without making the caller wait for the window") {
    CaptureDirectory  directory{"async"};
    std::atomic<bool> release = false;
    CaptureStore      store{directory.path, [&](std::chrono::milliseconds) -> CaptureResult {
                           while (!release)
                               std::this_thread::sleep_for(std::chrono::milliseconds{1});
                           return std::vector<std::uint8_t>{1, 2, 3};
                            }};
    GIVEN("a window that has not answered yet") {
        store.beginScreenshot("slow");  // returns at once
        THEN("it is pending, a second one is refused, and the result arrives later") {
            REQUIRE_FALSE(store.pollScreenshot().has_value());
            REQUIRE_THROWS_AS(store.beginScreenshot("other"), std::logic_error);
            release = true;
            std::optional<std::size_t> size;
            for (int attempt = 0; attempt < 500 && !(size = store.pollScreenshot()); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            REQUIRE(size == 3);
            REQUIRE(std::filesystem::exists(directory.path / "slow.png"));
            REQUIRE_THROWS_AS(store.pollScreenshot(), std::logic_error);
        }
    }
    GIVEN("an invalid name") {
        REQUIRE_THROWS_AS(store.beginScreenshot("../x"), std::invalid_argument);
    }
    release = true;
}

SCENARIO("An asynchronous screenshot reports a capture failure when polled") {
    CaptureDirectory directory{"asyncfail"};
    CaptureStore     store{directory.path, [](std::chrono::milliseconds) -> CaptureResult {
                           return std::unexpected("no window");
                           }};
    store.beginScreenshot("x");
    std::optional<std::size_t> size;
    REQUIRE_THROWS_WITH(
        [&] {
            for (int attempt = 0; attempt < 500 && !(size = store.pollScreenshot()); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }(),
        Catch::Matchers::Equals("Capture failed: no window"));
}

SCENARIO("A recording stops on request and reports its count by polling") {
    CaptureDirectory directory{"stop"};
    CaptureStore     store{directory.path, fakeImage};
    store.startRecording("run", 30);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    store.requestStop();
    std::optional<std::size_t> frames;
    for (int attempt = 0; attempt < 500 && !(frames = store.pollRecording()); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    REQUIRE(frames.has_value());
    REQUIRE(*frames >= 2);
    REQUIRE_THROWS_AS(store.requestStop(), std::logic_error);
    REQUIRE_THROWS_AS(store.pollRecording(), std::logic_error);
}

SCENARIO("A recording ends by itself at the image limit and can still be collected") {
    CaptureDirectory directory{"limit"};
    CaptureStore     store{directory.path, fakeImage, 3};
    store.startRecording("run", 30);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    store.requestStop();  // already finished: not an error
    std::optional<std::size_t> frames;
    for (int attempt = 0; attempt < 500 && !(frames = store.pollRecording()); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    REQUIRE(frames == 3);
    REQUIRE_FALSE(std::filesystem::exists(directory.path / "run" / "0004.png"));
    store.startRecording("again", 30);  // the slot is free once collected
    REQUIRE(store.stopRecording() <= 3);
}

SCENARIO("Capture callbacks of a Jasmine session obey the same page authority") {
    CaptureDirectory directory{"session"};
    ReportFile       report{"capture"};
    auto             store = std::make_shared<CaptureStore>(directory.path, fakeImage);
    JasmineSession   session{{"x"}, report.path, store};
    REQUIRE(session.attach(1));
    THEN("the instrumented page captures and another page cannot") {
        session.screenshotStart(1, "ok");
        REQUIRE(pollUntilDone([&] {
                    return session.screenshotPoll(1);
                })
                == 4);
        REQUIRE_THROWS(session.screenshotStart(2, "no"));
        REQUIRE_THROWS(session.screenshotPoll(2));
        REQUIRE_THROWS(session.recordStart(2, "no", 10));
        REQUIRE_THROWS(session.recordStop(2));
        REQUIRE_THROWS(session.recordPoll(2));
    }
    THEN("a recording is stopped and collected through the session") {
        session.recordStart(1, "clip", 30);
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        session.recordStop(1);
        REQUIRE(pollUntilDone([&] {
                    return session.recordPoll(1);
                })
                >= 2);
    }
    THEN("a session without a directory refuses to capture") {
        JasmineSession bare{{"x"}, report.path};
        REQUIRE(bare.attach(1));
        REQUIRE_THROWS(bare.screenshotStart(1, "ok"));
    }
}

SCENARIO("A window registration is replaced by a later one and never removes its successor") {
    using webfront::frontend::capturePng;
    using webfront::frontend::CaptureRegistration;
    const auto answer = [](std::uint8_t byte) {
        return [byte](std::chrono::milliseconds) -> CaptureResult {
            return std::vector<std::uint8_t>{byte};
        };
    };
    REQUIRE_FALSE(capturePng().has_value());  // nothing registered
    {
        CaptureRegistration first{answer(1)};
        REQUIRE(capturePng().value() == std::vector<std::uint8_t>{1});
        {
            CaptureRegistration second{answer(2)};
            REQUIRE(capturePng().value() == std::vector<std::uint8_t>{2});
            first = CaptureRegistration{};  // the stale one goes away
            REQUIRE(capturePng().value() == std::vector<std::uint8_t>{2});
            CaptureRegistration moved{std::move(second)};
            REQUIRE(capturePng().value() == std::vector<std::uint8_t>{2});
        }
        REQUIRE_FALSE(capturePng().has_value());  // the last owner closed the window
    }
}

SCENARIO("The Jasmine configuration accepts an optional capture directory") {
    using webfront::testing::detail::jasmineConfiguration;
    REQUIRE(jasmineConfiguration(R"({"files":["j","s"],"report":"r","captures":"c"})").captures == "c");
    REQUIRE(jasmineConfiguration(R"({"files":["j","s"],"report":"r"})").captures.empty());
    REQUIRE_THROWS(jasmineConfiguration(R"({"files":["j","s"],"report":"r","captures":"a","captures":"b"})"));
}
