#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <components/ToolManager.hpp>
#include <http/Encodings.hpp>
#include <http/HTTPServer.hpp>
#include <networking/NetworkingMock.hpp>

#include <any>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace std;
using namespace webfront;
using webfront::test::TemporaryDirectory;

namespace {

// The project directory of the tools, chosen by each test.
struct TestProject {
    static inline filesystem::path directory;
    static filesystem::path        get() {
        return directory;
    }
};

using TestPackageFS = fs::BasicPackageFS<TestProject>;

// Records the functions registered on it, like BasicWF::cppFunction, and calls them directly.
struct RecordingApp {
    map<string, any> functions;

    template <typename R, typename... Args>
    void cppFunction(string name, auto&& function) {
        functions.try_emplace(std::move(name), std::function<R(Args...)>(std::forward<decltype(function)>(function)));
    }

    template <typename R, typename... Args>
    R call(const string& name, Args... args) {
        return any_cast<std::function<R(Args...)>&>(functions.at(name))(std::move(args)...);
    }

    tuple<string, string, string> runLine(const string& line) {
        return call<tuple<string, string, string>, string>("webfront.terminal.run", line);
    }
    string run(const string& line) {
        return get<0>(runLine(line));
    }
};

vector<uint8_t> bytes(string_view text) {
    return {text.begin(), text.end()};
}

optional<string> served(TestPackageFS& packages, const filesystem::path& path) {
    auto file = packages.open(path);
    if (!file)
        return nullopt;
    string         content;
    array<char, 4> buffer{};
    while (!file->eof())
        content.append(buffer.data(), file->read(buffer));
    return content;
}

bool contains(const string& text, const string& part) {
    return text.find(part) != string::npos;
}

}  // namespace

SCENARIO("The tool manager installs tools through its bridge functions") {
    GIVEN("a terminal with a tool manager on an application") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        RecordingApp app;
        Terminal     terminal{app};
        auto         manager = make_unique<ToolManager<RecordingApp, TestProject>>(app, terminal);
        REQUIRE(manager->store().projectDir() == project.path());

        WHEN("the page installs a tool in two chunks") {
            app.call<void, string, string>("webfront.pkg.begin", "@acme/plot", "1.2.0");
            app.call<void, string, string, string, double, vector<uint8_t>>("webfront.pkg.writeFile",
                                                                            "@acme/plot",
                                                                            "1.2.0",
                                                                            "dist/index.html",
                                                                            0,
                                                                            bytes("<p>pl"));
            app.call<void, string, string, string, double, vector<uint8_t>>("webfront.pkg.writeFile",
                                                                            "@acme/plot",
                                                                            "1.2.0",
                                                                            "dist/index.html",
                                                                            5,
                                                                            bytes("ot</p>"));
            app.call<void, string, string, string, string, string>("webfront.pkg.commit", "@acme/plot", "1.2.0", "sha512-abc", "dist/index.html", "Live plot");

            THEN("the page lists it with the URL of its entry") {
                REQUIRE(app.call<string, double>("webfront.pkg.list", 0) == "@acme/plot\t1.2.0\t/webfront/tools/@acme/plot/dist/index.html\tLive plot\n");
            }
            THEN("wf list shows it") {
                REQUIRE(app.run("wf list") == "@acme/plot@1.2.0  Live plot");
            }
            THEN("PackageFS serves its files") {
                TestPackageFS packages{"ignored"};
                REQUIRE(served(packages, "webfront/tools/@acme/plot/dist/index.html") == "<p>plot</p>");
                REQUIRE(served(packages, "/webfront/tools/@acme/plot/dist/index.html") == "<p>plot</p>");
            }
            AND_WHEN("wf remove uninstalls it") {
                REQUIRE(app.run("wf remove @acme/plot") == "Removed @acme/plot.");
                THEN("it is no longer listed nor served") {
                    REQUIRE(app.run("wf list") == "No tool installed.");
                    REQUIRE(app.call<string, double>("webfront.pkg.list", 0).empty());
                    TestPackageFS packages{"ignored"};
                    REQUIRE_FALSE(served(packages, "webfront/tools/@acme/plot/dist/index.html"));
                }
            }
            AND_WHEN("the ToolManager object is destroyed") {
                manager.reset();
                THEN("the registered functions still own the store") {
                    REQUIRE(contains(app.run("wf list"), "@acme/plot@1.2.0"));
                }
            }
        }
    }
}

SCENARIO("Installed tools are listed with URLs the HTTP server resolves") {
    GIVEN("a terminal with a tool manager on an application") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        RecordingApp                           app;
        Terminal                               terminal{app};
        ToolManager<RecordingApp, TestProject> manager{app, terminal};

        WHEN("a tool whose entry needs escaping in a URL is installed") {
            const string entry = "dist/a+b c%41\xc3\xa9#[].html";
            app.call<void, string, string>("webfront.pkg.begin", "logs", "1.0.0");
            app.call<void, string, string, string, double, vector<uint8_t>>("webfront.pkg.writeFile", "logs", "1.0.0", entry, 0, bytes("<p>logs</p>"));
            app.call<void, string, string, string, string, string>("webfront.pkg.commit", "logs", "1.0.0", "", entry, "Logs");
            THEN("its listed URL is absolute, encoded, and leads the server to the file") {
                const auto list = app.call<string, double>("webfront.pkg.list", 0);
                const auto url  = list.substr(list.find("logs\t1.0.0\t") + 11);
                REQUIRE(url.starts_with("/webfront/tools/logs/dist/a%2Bb%20c%2541%C3%A9%23%5B%5D.html\t"));
                TestPackageFS packages{"ignored"};
                REQUIRE(served(packages, uri::decode(url.substr(0, url.find('\t')))) == "<p>logs</p>");
                http::RequestHandler<networking::NetworkingMock, TestPackageFS> handler{"."};
                http::Request                                                   request;
                request.method = http::Request::Method::Get;
                request.uri    = url.substr(0, url.find('\t'));
                REQUIRE(handler.handleRequest(request).statusCode == http::Response::ok);
            }
        }
    }
}

SCENARIO("The tool manager rejects malformed calls and commands") {
    GIVEN("a terminal with a tool manager on an application") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        RecordingApp                           app;
        Terminal                               terminal{app};
        ToolManager<RecordingApp, TestProject> manager{app, terminal};

        THEN("a negative, fractional or too large offset is rejected") {
            app.call<void, string, string>("webfront.pkg.begin", "plot", "1.0.0");
            for (const double offset : {-1.0, 0.5, 1e300, numeric_limits<double>::infinity(), numeric_limits<double>::quiet_NaN()})
                REQUIRE_THROWS_AS(
                    (app.call<void, string, string, string, double, vector<uint8_t>>("webfront.pkg.writeFile", "plot", "1.0.0", "a.js", offset, bytes("x"))),
                    invalid_argument);
        }
        THEN("wf documents itself and rejects malformed commands") {
            REQUIRE(contains(app.run("help"), "Manage web tools: wf search | install | list | open | remove"));
            REQUIRE(contains(app.run("wf"), "wf install <package>[@<version>]"));
            REQUIRE(contains(app.run("wf"), "wf remove <package>"));
            REQUIRE(app.run("wf help") == app.run("wf"));
            REQUIRE_THROWS_AS(app.run("wf frobnicate"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf remove"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf remove a b"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf list all"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf remove plot"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf remove ../x"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf install"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf install a b"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf install Plot"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf install ../x"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf install plot@"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf open"), invalid_argument);
            REQUIRE_THROWS_AS(app.run("wf open plot"), invalid_argument);
        }
    }
}

SCENARIO("The tool list is paged below the bridge's string limit") {
    GIVEN("more tools with long titles than one bridge string can carry") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        RecordingApp                           app;
        Terminal                               terminal{app};
        ToolManager<RecordingApp, TestProject> manager{app, terminal};
        constexpr size_t                       toolCount = 70;
        for (size_t index = 0; index < toolCount; ++index) {
            const auto name = "tool" + to_string(index);
            manager.store().begin(name, "1.0.0");
            manager.store().write(name, "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("x")});
            manager.store().commit({.name = name, .version = "1.0.0", .entry = "index.html", .title = string(1024, 't')});
        }

        WHEN("the page reads the list from index 0 until an empty page") {
            vector<string> lines;
            size_t         pages = 0;
            for (string page = app.call<string, double>("webfront.pkg.list", 0); !page.empty();
                 page        = app.call<string, double>("webfront.pkg.list", static_cast<double>(lines.size()))) {
                REQUIRE(page.size() <= tools::detail::maxListBytes);
                REQUIRE(page.ends_with('\n'));
                for (size_t start = 0, end = page.find('\n'); end != string::npos; start = end + 1, end = page.find('\n', start))
                    lines.push_back(page.substr(0, page.find('\t', start)).substr(start));
                ++pages;
            }
            THEN("every tool is listed once, over several pages") {
                REQUIRE(pages > 1);
                REQUIRE(lines.size() == toolCount);
                REQUIRE(set<string>(lines.begin(), lines.end()).size() == toolCount);
            }
        }
        THEN("an index past the end gives an empty page and an invalid index is rejected") {
            REQUIRE(app.call<string, double>("webfront.pkg.list", toolCount).empty());
            REQUIRE_THROWS_AS((app.call<string, double>("webfront.pkg.list", -1)), invalid_argument);
        }
    }
}

SCENARIO("PackageFS serves only the files of installed tools") {
    GIVEN("a project with one installed tool and a file outside it") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        fs::ToolStore store{project.path()};
        store.begin("plot", "1.0.0");
        store.write("plot", "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("<p>plot</p>")});
        store.write("plot", "1.0.0", {.path = "assets/app.js", .offset = 0, .bytes = bytes("go()")});
        store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"});
        ofstream(project.path() / "secret.txt") << "secret";
        TestPackageFS packages{"ignored"};

        THEN("files of the installed version are served, also in subdirectories") {
            REQUIRE(served(packages, "webfront/tools/plot/index.html") == "<p>plot</p>");
            REQUIRE(served(packages, "webfront/tools/plot/assets/app.js") == "go()");
        }
        THEN("missing files, directories, unknown tools and other prefixes are not served") {
            REQUIRE_FALSE(served(packages, "webfront/tools/plot/missing.js"));
            REQUIRE_FALSE(served(packages, "webfront/tools/plot/assets"));
            REQUIRE_FALSE(served(packages, "webfront/tools/other/index.html"));
            REQUIRE_FALSE(served(packages, "webfront/tools/plot"));
            REQUIRE_FALSE(served(packages, "webfront/tools/@scope"));
            REQUIRE_FALSE(served(packages, "tools/plot/index.html"));
        }
        THEN("paths escaping the tool are not served") {
            REQUIRE_FALSE(served(packages, "webfront/tools/plot/../../../secret.txt"));
            REQUIRE_FALSE(served(packages, "webfront/tools/plot/./index.html"));
            REQUIRE_FALSE(served(packages, "webfront/tools/../../secret.txt"));
        }
        WHEN("the lock file becomes malformed") {
            ofstream(project.path() / "webfront-tools.lock.json") << "{";
            THEN("nothing is served") {
                REQUIRE_FALSE(served(packages, "webfront/tools/plot/index.html"));
            }
        }
    }
}

SCENARIO("ToolsApp serves installed tools after the protected bridge script") {
    GIVEN("the filesystem of ToolsApp") {
        fs::DefaultFSWith<fs::TerminalFS, fs::PackageFS> files{""};
        THEN("the bridge script, the terminal and the fallback page are still served") {
            REQUIRE(files.open("WebFront.js"));
            REQUIRE(files.open(fs::TerminalFS::page));
            REQUIRE(files.open("index.html"));
        }
        THEN("an unknown tool falls through to the next providers") {
            REQUIRE_FALSE(files.open("webfront/tools/none/index.html"));
        }
    }
}

SCENARIO("wf search, install and open continue in the tool manager's browser module") {
    GIVEN("a tool manager with a local registry and one installed tool") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        RecordingApp                           app;
        Terminal                               terminal{app};
        ToolManager<RecordingApp, TestProject> manager{app, terminal, {.registry = "/registry"}};
        manager.store().begin("@acme/plot", "1.2.0");
        manager.store().write("@acme/plot", "1.2.0", {.path = "dist/main page.html", .bytes = bytes("<p>")});
        manager.store().commit({.name = "@acme/plot", .version = "1.2.0", .integrity = "sha512-abc", .entry = "dist/main page.html", .title = "Plot"});
        const string module{fs::PackageFS::toolManagerModule};

        THEN("wf search passes the registry and the joined term") {
            REQUIRE(app.runLine("wf search live  plot") == tuple<string, string, string>{"", module, "search\0/registry\0live plot\0"s});
            REQUIRE(app.runLine("wf search") == tuple<string, string, string>{"", module, "search\0/registry\0\0"s});
        }
        THEN("wf install passes the registry, the name and the requested version") {
            REQUIRE(get<2>(app.runLine("wf install @acme/plot@2.0.0")) == "install\0/registry\0@acme/plot\0002.0.0\0"s);
            REQUIRE(get<2>(app.runLine("wf install plot@next")) == "install\0/registry\0plot\0next\0"s);
            REQUIRE(get<2>(app.runLine("wf install @acme/plot")) == "install\0/registry\0@acme/plot\0\0"s);
        }
        THEN("wf open passes the encoded URL of the installed tool's entry") {
            REQUIRE(app.runLine("wf open @acme/plot")
                    == tuple<string, string, string>{"", module, "open\0/webfront/tools/@acme/plot/dist/main%20page.html\0"s});
        }
    }
}

SCENARIO("PackageFS serves the tool manager's browser module") {
    GIVEN("a PackageFS without installed tools") {
        TemporaryDirectory project;
        TestProject::directory = project.path();
        TestPackageFS packages{"ignored"};
        THEN("the module is served at its absolute path") {
            const string path{fs::PackageFS::toolManagerModule};
            auto         module = packages.open(path);
            REQUIRE(module);
            REQUIRE(packages.open(path.substr(1)));
            string            content;
            array<char, 4096> buffer{};
            while (const auto bytesRead = module->read(buffer))
                content.append(buffer.data(), bytesRead);
            REQUIRE(contains(content, "export async function run("));
        }
    }
}
