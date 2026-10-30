#include <catch2/catch_test_macros.hpp>
#include <webfront/testing/Jasmine.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

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
        JasmineSession session{{std::string(chunk * 2, 'a'), "", "short"}, report.path};
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
            for (const auto& [index, offset] : {std::pair{-1.0, 0.0}, {3.0, 0.0}, {0.5, 0.0}, {0.0, -1.0}, {0.0, 0.5},
                                               {0.0, static_cast<double>(chunk * 2 + 1)}, {std::nan(""), 0.0}, {0.0, std::nan("")},
                                               {0.0, HUGE_VAL}})
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
