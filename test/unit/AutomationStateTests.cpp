// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-28
//
// WebFront — tests/unit
// Role: The inspectors' reads as MCP tools (ADR-027, section 10): catalog, grant, reading a watch, bounds and refusals.
// Thread: bridge-io | Alloc: unrestricted
// LLM: the tools are read-only; a test that expects a write tool here is wrong

#include "TemporaryDirectory.hpp"
#include <webfront/automation/Enable.hpp>
#include <webfront/DeveloperApp.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/testing/Bdd.hpp>

#include <map>
#include <string>
#include <vector>

using namespace webfront::bdd;

namespace statetests {
using namespace webfront;
using namespace webfront::automation;

struct QuietFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
};
/// A developer application on the mock network: a task it spawns runs at once, on the calling thread.
using StateApp = DeveloperAppWithNetwork<networking::NetworkingMock, QuietFrontend>;

/// An answer may be as long as the inspectors' budget allows, which is more than a request's 60 000 bytes.
constexpr std::size_t answerLimit = 4 * 1024 * 1024;

/// An MCP conversation with a session, already initialized.
struct Client {
    explicit Client(Session& controller) : session(controller), mcp(controller) {
        static_cast<void>(mcp.receive(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"test","version":"1"},"capabilities":{}}})"));
        static_cast<void>(mcp.receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    std::string tools() {
        return Object(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})")).raw("result")).raw("tools");
    }
    /// The whole JSON-RPC answer of a tool call whose arguments are `members` and the session's identifier (or `as`).
    Object answer(std::string_view tool, std::map<std::string, std::string> members = {}, std::string as = {}) {
        members["session"] = json::quote(as.empty() ? Object(session.describe()).string("session") : as);
        return Object(*mcp.receive(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":)" + json::quote(tool)
                                   + ",\"arguments\":" + objectJson(members) + "}}"),
                      answerLimit);
    }
    /// The result of a tool call: `isError` and `structuredContent`.
    Object call(std::string_view tool, std::map<std::string, std::string> members = {}, std::string as = {}) {
        return Object(answer(tool, std::move(members), std::move(as)).raw("result"), answerLimit);
    }
    /// The structured answer of a tool call that must succeed.
    Object read(std::string_view tool, std::map<std::string, std::string> members = {}) {
        const auto result = call(tool, std::move(members));
        requireEqual(result.raw("isError"), std::string{"false"});
        return Object(result.raw("structuredContent"), answerLimit);
    }
    /// The code of a tool call that must fail.
    std::string refused(std::string_view tool, std::map<std::string, std::string> members = {}, std::string as = {}) {
        const auto result = call(tool, std::move(members), std::move(as));
        requireEqual(result.raw("isError"), std::string{"true"});
        return Object(result.raw("structuredContent")).string("code");
    }
    Session&     session;
    Mcp<Session> mcp;
};

/// The objects of a JSON array, in order.
std::vector<Object> rows(std::string_view array) {
    std::vector<Object> values;
    json::Reader        reader{array};
    reader.array([&] {
        values.emplace_back(reader.rawValue());
    });
    reader.end();
    return values;
}

AutomationOptions quietly(std::vector<std::string> grants, const std::filesystem::path& sessions) {
    return {.grants = std::move(grants), .discovery = sessions, .announce = false};
}

constexpr auto environment = "https://example.test/env.tgz";
}  // namespace statetests

const Scenario stateToolsRead{
    "The inspectors' reads are MCP tools of an application that granted `state` (ADR-027, section 10)",
    {"automation", "inspector"},
    [] {
        using namespace statetests;
        webfront::test::TemporaryDirectory root;
        std::filesystem::create_directory(root.path() / "project");
        StateApp app(root.path() / "project", WebFrontConfig{.port = "0"});
        app.bootstrap("developer", {.environment = environment, .integrity = "sha512-abc"});
        app.cppFunction("app.greet", [](std::string who) {
            return "Hello, " + who;
        });
        std::vector<std::string> names{"ada", "grace", "edsger"};
        std::string              essay(200'000, 'x');
        auto                     watched = app.watch("names", [&]() -> const std::vector<std::string>& {
            return names;
        });
        auto                     written = app.watch("essay", [&](inspect::Visit<std::string> visit) {
            visit(essay);
        });
        require(static_cast<bool>(watched));
        auto   automation = enableAutomation(app, quietly({"state"}, root.path() / "sessions"));
        Client client{*automation->controller()};

        given("the list of the tools", [&] {
            const auto tools = client.tools();
            then("the four tools of the session and the ten reads of the state are named", [&] {
                for (const auto* name : {"webfront.session", "webfront.start", "webfront.result", "webfront.cancel"})
                    require(tools.find(name) != std::string::npos);
                for (const auto name : state::toolNames)
                    require(tools.find("\"" + std::string(name) + "\"") != std::string::npos);
                requireEqual(rows(tools).size(), std::size_t{14});
            });
            then("every read is declared read-only and takes the session, and none writes", [&] {
                for (const auto& tool : rows(tools)) {
                    if (!tool.string("name").starts_with(state::toolPrefix))
                        continue;
                    requireEqual(Object(tool.raw("annotations")).raw("readOnlyHint"), std::string{"true"});
                    require(Object(tool.raw("inputSchema")).raw("required").find("\"session\"") != std::string::npos);
                }
                for (const auto& tool : rows(tools))
                    require(tool.string("name").find("write") == std::string::npos);
                for (const auto* name : {"webfront.state.write", "webfront.inspector.write", "webfront.inspector.children"})
                    require(client.answer(name).has("error"));
            });
        });

        given("the watches of the application", [&] {
            const auto listed  = client.read("webfront.state.watches");
            const auto watches = rows(listed.raw("watches"));
            then("they are discovered with their name, kind, place of declaration and whether they can be written", [&] {
                requireEqual(watches.size(), std::size_t{2});
                requireEqual(watches[0].string("name"), std::string{"names"});
                requireEqual(watches[0].raw("writable"), std::string{"false"});
                requireEqual(watches[1].raw("writable"), std::string{"true"});
                require(watches[0].string("location").find("AutomationStateTests.cpp") != std::string::npos);
                requireEqual(listed.raw("next"), std::string{"0"});
                requireEqual(listed.raw("changed"), std::string{"true"});
                const auto again = client.read("webfront.state.watches",
                                               {
                                                   {"revision", listed.raw("revision")}});
                requireEqual(again.raw("changed"), std::string{"false"});
            });
            const auto watch = watches[0].raw("id");
            when("the root is unfolded with the revision 0", [&] {
                const auto first = client.read("webfront.state.children",
                                               {
                                                   {"watch", watch}});
                then("the answer asks to synchronize and gives the revision to use", [&] {
                    requireEqual(first.raw("resynchronize"), std::string{"true"});
                    require(rows(first.raw("nodes")).empty());
                });
                const auto page  = client.read("webfront.state.children",
                                               {
                                                   {"watch", watch},
                                                   {"revision", first.raw("revision")}});
                const auto nodes = rows(page.raw("nodes"));
                then("the page gives the root, then its elements", [&] {
                    requireEqual(nodes.size(), std::size_t{4});
                    requireEqual(nodes[0].string("path"), std::string{});
                    requireEqual(nodes[0].raw("children"), std::string{"3"});
                    requireEqual(nodes[1].string("value"), std::string{"ada"});
                    requireEqual(nodes[3].string("value"), std::string{"edsger"});
                    requireEqual(nodes[1].raw("writable"), std::string{"false"});
                    requireEqual(page.raw("next"), std::string{"0"});
                });
                then("a page is bounded by its count, and its `next` gives the following one", [&] {
                    const auto fresh = client.read("webfront.state.reset",
                                                   {
                                                       {"watch", watch}});
                    const auto one   = client.read("webfront.state.children",
                                                   {
                                                       {"watch", watch},
                                                       {"revision", fresh.raw("revision")},
                                                       {"count", "2"}});
                    requireEqual(rows(one.raw("nodes")).size(), std::size_t{3});  // the parent, then two children
                    requireEqual(one.raw("next"), std::string{"2"});
                    const auto two = client.read("webfront.state.children",
                                                 {
                                                     {"watch", watch},
                                                     {"revision", one.raw("revision")},
                                                     {"start", one.raw("next")},
                                                     {"count", "2"}});
                    requireEqual(rows(two.raw("nodes")).back().string("value"), std::string{"edsger"});
                    requireEqual(two.raw("next"), std::string{"0"});
                });
                then("a pass of differences tells what the application changed since", [&] {
                    names[1]           = "barbara";
                    const auto changes = client.read("webfront.state.changes",
                                                     {
                                                         {"watch", watch},
                                                         {"revision", page.raw("revision")}});
                    const auto changed = rows(changes.raw("nodes"));
                    requireEqual(changed.size(), std::size_t{1});
                    requireEqual(changed[0].string("value"), std::string{"barbara"});
                    requireEqual(changed[0].string("path"), nodes[2].string("path"));
                    requireEqual(changes.raw("resynchronize"), std::string{"false"});
                });
                then("folding the root frees the memory of the reads and leaves the application as it is", [&] {
                    const auto folded = client.read("webfront.state.fold",
                                                    {
                                                        {"watch", watch},
                                                        {"path", "\"\""},
                                                        {"revision", page.raw("revision")}});
                    require(folded.raw("revision") != page.raw("revision"));
                    requireEqual(names.size(), std::size_t{3});
                });
            });
            when("a value longer than a page shows is read whole", [&] {
                const auto essayWatch = watches[1].raw("id");
                const auto opened     = client.read("webfront.state.children",
                                                    {
                                                        {"watch", essayWatch}});
                const auto top        = client.read("webfront.state.children",
                                                    {
                                                        {"watch", essayWatch},
                                                        {"revision", opened.raw("revision")}});
                then("its node is marked truncated, and the fragments of one snapshot give all of it", [&] {
                    requireEqual(rows(top.raw("nodes")).front().raw("truncated"), std::string{"true"});
                    std::string text, snapshot = "0", offset = "0";
                    std::size_t fragments = 0;
                    do {
                        const auto part = client.read("webfront.state.value",
                                                      {
                                                          {"watch", essayWatch},
                                                          {"path", "\"\""},
                                                          {"snapshot", snapshot},
                                                          {"offset", offset}});
                        text           += part.string("text");
                        snapshot        = part.raw("snapshot");
                        offset          = part.raw("next");
                        requireEqual(part.raw("size"), std::to_string(essay.size()));
                        ++fragments;
                    } while (offset != "0");
                    require(text == essay);
                    require(fragments > 1);  // each fragment stays within the answer's budget
                });
                then("a fragment asked with an identifier that is not the snapshot's is refused as stale", [&] {
                    requireEqual(client.refused("webfront.state.value",
                                                {
                                                    {"watch", essayWatch},
                                                    {"path", "\"\""},
                                                    {"snapshot", "99"},
                                                    {"offset", "10"}}),
                                 std::string{"webfront.inspector.stale"});
                });
            });
            when("the watch is removed", [&] {
                watched = {};
                then("its reads answer the terminal refusal of the inspectors", [&] {
                    requireEqual(client.refused("webfront.state.children",
                                                {
                                                    {"watch", watch}}),
                                 std::string{"webfront.inspector.unknown"});
                });
            });
        });

        given("the registry of the application", [&] {
            then("the registered functions are listed with their TypeScript signature and where they were registered", [&] {
                const auto functions = rows(client.read("webfront.state.functions").raw("functions"));
                const auto greet     = std::ranges::find_if(functions, [](const Object& row) {
                    return row.string("name") == "app.greet";
                });
                require(greet != functions.end());
                requireEqual(greet->string("signature"), std::string{"(a: string) => Promise<string>"});
                require(greet->string("location").find("AutomationStateTests.cpp") != std::string::npos);
                requireEqual(greet->raw("calls"), std::string{"0"});
                for (const auto& row : functions)
                    require(!row.string("name").starts_with("webfront.inspector."));
            });
            then("a page of functions is bounded by its count and continued by its cursor", [&] {
                const auto one = client.read("webfront.state.functions",
                                             {
                                                 {"count", "1"}});
                requireEqual(rows(one.raw("functions")).size(), std::size_t{1});
                require(one.raw("next") != "0");
                const auto two = client.read("webfront.state.functions",
                                             {
                                                 {"after", one.raw("next")},
                                                 {"count", "1"}});
                require(rows(two.raw("functions")).front().raw("id") != rows(one.raw("functions")).front().raw("id"));
            });
            then("the calls to JavaScript, the browsers and the tasks are read without a page", [&] {
                require(rows(client.read("webfront.state.jsFunctions").raw("functions")).empty());
                const auto browsers = client.read("webfront.state.browsers");
                require(rows(browsers.raw("browsers")).empty());
                requireEqual(browsers.raw("tasks"), std::string{"0"});
            });
            then("the bounds are those of the inspectors, without the bound of a write", [&] {
                const auto limits = client.read("webfront.state.limits");
                requireEqual(limits.raw("maxNodes"), std::string{"10000"});
                requireEqual(limits.raw("maxSnapshotBytes"), std::to_string(16 * 1024 * 1024));
                requireEqual(limits.raw("defaultPageCount"), std::string{"200"});
                requireEqual(limits.raw("responseBudget"), std::to_string(inspect::responseBudget<http::DefaultBuffersPolicy>));
                require(!limits.has("maxWriteTextBytes"));
            });
        });

        given("arguments that are not what a read takes", [&] {
            then("they are refused as invalid arguments, without reaching the application", [&] {
                for (const auto& members : std::vector<std::map<std::string, std::string>>{
                         {{"watch", "\"1\""}},
                         {{"watch", "1.5"}},
                         {{"watch", "1"}, {"count", "-1"}},
                         {{"watch", "9007199254740993"}},
                         {{"watch", "1"}, {"start", "9007199254740993"}},
                         {{"watch", "1"}, {"start", "9.007199254740993e15"}},
                         {{"watch", "0"}},
                         {{"watch", "1"}, {"count", "1001"}},
                         {{"watch", "1"}, {"count", "1e9"}},
                         {{"watch", "1"}, {"path", "7"}},
                         {{"watch", "1"}, {"text", "\"x\""}},
                         {}})
                    requireEqual(client.refused("webfront.state.children", members), std::string{"invalid_arguments"});
                requireEqual(client.refused("webfront.state.limits",
                                            {
                                                {"watch", "1"}}),
                             std::string{"invalid_arguments"});
                // The bounds that tools/list announces, checked before the application is reached.
                requireEqual(client.refused("webfront.state.reset",
                                            {
                                                {"watch", "0"}}),
                             std::string{"invalid_arguments"});
                requireEqual(client.refused("webfront.state.functions",
                                            {
                                                {"count", "1001"}}),
                             std::string{"invalid_arguments"});
            });
            then("a whole number may be written as JSON allows it, 1.0 or 1e0", [&] {
                for (const auto* spelling : {"1.0", "1e0", "10E-1"})
                    requireEqual(rows(client
                                          .read("webfront.state.functions",
                                                {
                                                    {"count", spelling}})
                                          .raw("functions"))
                                     .size(),
                                 std::size_t{1});
            });
            then("another session's identifier is refused and audited", [&] {
                requireEqual(client.refused("webfront.state.watches", {}, "someone-else"), std::string{"wrong_session"});
                const auto audit = Object(automation->controller()->describe()).raw("audit");
                require(audit.find("\"denied\"") != std::string::npos);
                require(audit.find("webfront.state.watches") != std::string::npos);
            });
        });

        given("a session that was closed", [&] {
            automation->close();
            then("its reads are refused", [&] {
                requireEqual(client.refused("webfront.state.watches"), std::string{"not_ready"});
            });
        });
      }
};

const Scenario stateToolsNeedTheGrant{
    "The reads of the state exist only with the `state` grant and the developer environment",
    {"automation", "inspector"},
    [] {
        using namespace statetests;
        webfront::test::TemporaryDirectory root;
        std::filesystem::create_directory(root.path() / "project");
        given("an application that did not grant `state`", [&] {
            StateApp app(root.path() / "project", WebFrontConfig{.port = "0"});
            app.bootstrap("developer", {.environment = environment, .integrity = "sha512-abc"});
            int    counter    = 0;
            auto   watched    = app.watch("counter", [&]() -> const int& {
                return counter;
            });
            auto   automation = enableAutomation(app, quietly({"check", "inspect"}, root.path() / "sessions"));
            Client client{*automation->controller()};
            then("the tools are the four of the session", [&] {
                const auto tools = client.tools();
                requireEqual(rows(tools).size(), std::size_t{4});
                require(tools.find("webfront.state.") == std::string::npos);
            });
            then("a read asked through MCP is an unknown tool, and its refusal is audited", [&] {
                require(Object(automation->controller()->describe()).raw("audit").find("webfront.state.watches") == std::string::npos);
                requireEqual(Object(client.answer("webfront.state.watches").raw("error")).string("message"), std::string{"Unknown tool"});
                const auto audit = Object(automation->controller()->describe()).raw("audit");
                require(audit.find("\"denied\"") != std::string::npos);
                require(audit.find("webfront.state.watches") != std::string::npos);
            });
            then("the controller refuses a read too", [&] {
                const auto identity = Object(automation->controller()->describe()).string("session");
                try {
                    static_cast<void>(automation->controller()->read("webfront.state.watches", Object("{\"session\":" + json::quote(identity) + "}")));
                    require(false);
                } catch (const OperationError& refused) {
                    requireEqual(refused.code, std::string{"not_authorized"});
                }
                require(Object(automation->controller()->describe()).raw("audit").find("webfront.state.watches") != std::string::npos);
            });
        });
        given("an application that did not mount the developer environment", [&] {
            StateApp app(root.path() / "project", WebFrontConfig{.port = "0"});
            then("it has no reads, and the `state` grant is refused when automation is enabled", [&] {
                require(app.inspectorReads() == nullptr);
                requireThrows([&] {
                    static_cast<void>(enableAutomation(app, quietly({"state"}, root.path() / "sessions")));
                });
            });
        });
        given("a session built without a reader", [&] {
            then("the `state` grant is refused, and another grant is not", [&] {
                requireThrows([&] {
                    Session{
                        {root.path() / "project", {"state"}}};
                });
                checkNothrow([&] {
                    Session{
                        {root.path() / "project", {"check"}}};
                });
            });
        });
        given("a session whose reader answers what it was asked", [&] {
            std::string asked;
            Session     session{
                {.project = root.path() / "project", .allow = {"state"}, .state = [&](std::string_view tool, const Object& arguments) {
                     asked = std::string(tool) + objectJson(arguments.members());
                     return std::string{"{\"ok\":true}"};
                 }}};
            const auto identity = Object(session.describe()).string("session");
            then("the reader receives the tool and its arguments without the session identifier, before the window is ready", [&] {
                const auto answer = session.read("webfront.state.children", Object("{\"session\":" + json::quote(identity) + ",\"watch\":3}"));
                requireEqual(answer, std::string{"{\"ok\":true}"});
                requireEqual(asked, std::string{"webfront.state.children{\"watch\":3}"});
            });
            then("a refusal whose message is no UTF-8 text still answers JSON that a client can decode", [&] {
                Session failing{
                    {.project = root.path() / "project", .allow = {"state"}, .state = [](std::string_view, const Object&) -> std::string {
                         throw OperationError("webfront.inspector.unavailable", "source failed: \xFF\xC3");
                     }}};
                Client     client{failing};
                const auto result = client.call("webfront.state.watches");
                requireEqual(result.raw("isError"), std::string{"true"});
                requireEqual(Object(result.raw("structuredContent")).string("error"), std::string{"source failed: \xEF\xBF\xBD\xEF\xBF\xBD"});
                checkNothrow([&] {
                    validateInput(result.raw("structuredContent"));
                });
            });
            then("a read is not audited, so that polling does not evict the refusals", [&] {
                static_cast<void>(session.read("webfront.state.limits", Object("{\"session\":" + json::quote(identity) + "}")));
                requireEqual(Object(session.describe()).raw("audit"), std::string{"[]"});
            });
        });
      }
};

const Scenario packageReads{"The locked packages are read by operations of their own grant", {"automation"}, [] {
                                using namespace statetests;
                                webfront::test::TemporaryDirectory root;
                                const auto                         start = [](Session& session, std::string_view operation, std::string_view arguments) {
                                    return session.start(Object("{\"session\":" + json::quote(Object(session.describe()).string("session")) + ",\"operation\":"
                                                                + json::quote(operation) + ",\"arguments\":" + std::string(arguments) + "}"));
                                };
                                given("a session granted `packages`", [&] {
                                    Session session{
                                        {root.path(), {"packages"}}
                                    };
                                    session.poll();
                                    then("list takes no argument, why and info take one plain package name", [&] {
                                        checkNothrow([&] {
                                            static_cast<void>(start(session, "list", "{}"));
                                        });
                                        session.complete("run-1", R"({"exitCode":0,"output":"react@19.3.1"})");
                                        requireThrows([&] {
                                            static_cast<void>(start(session, "list", R"({"package":"react"})"));
                                        });
                                        for (const auto* operation : {"why", "info"}) {
                                            requireThrows([&] {
                                                static_cast<void>(start(session, operation, "{}"));
                                            });
                                            requireThrows([&] {
                                                static_cast<void>(start(session, operation, R"({"package":"--tree"})"));
                                            });
                                        }
                                        checkNothrow([&] {
                                            static_cast<void>(start(session, "why", R"({"package":"scheduler"})"));
                                        });
                                    });
                                    then("the page is told which environment runs them", [&] {
                                        const auto versioned = session.versionedOperations();
                                        requireEqual(versioned.size(), std::size_t{3});
                                        for (const auto& [operation, since] : versioned)
                                            requireEqual(since, std::string{"1.2.1"});
                                    });
                                });
                                given("a session granted `state` only", [&] {
                                    Session session{
                                        {.project = root.path(), .allow = {"state"}, .state = [](std::string_view, const Object&) {
                                             return std::string{"{}"};
                                         }}
                                    };
                                    session.poll();
                                    then("the package operations are not authorized, and no environment version is required", [&] {
                                        try {
                                            static_cast<void>(start(session, "list", "{}"));
                                            require(false);
                                        } catch (const OperationError& refused) {
                                            requireEqual(refused.code, std::string{"not_authorized"});
                                        }
                                        require(session.versionedOperations().empty());
                                    });
                                });
                            }};

const Scenario stateTexts{"A value read in the application is always valid JSON text", {"automation"}, [] {
                              using namespace webfront::automation;
                              then("bytes that are no UTF-8 become U+FFFD, and the rest is kept", [&] {
                                  requireEqual(state::detail::text("caf\xC3\xA9"), std::string{"\"caf\xC3\xA9\""});
                                  requireEqual(state::detail::text("a\xFFz"), std::string{"\"a\xEF\xBF\xBDz\""});
                                  requireEqual(state::detail::text("cut\xE2\x82"), std::string{"\"cut\xEF\xBF\xBD\xEF\xBF\xBD\""});
                                  requireEqual(state::detail::text("\xC0\xAF"), std::string{"\"\xEF\xBF\xBD\xEF\xBF\xBD\""});
                                  requireEqual(state::detail::text("\xED\xA0\x80"), std::string{"\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\""});
                                  requireEqual(state::detail::text("q\"\n"), std::string{"\"q\\\"\\u000a\""});
                                  checkNothrow([&] {
                                      Object("{\"v\":" + state::detail::text(std::string("\x80\xF5\xE2\x28\xA1", 5)) + "}");
                                  });
                              });
                              then("numbers keep whole values exact and never give a text JSON refuses", [&] {
                                  requireEqual(state::detail::number(0), std::string{"0"});
                                  requireEqual(state::detail::number(9007199254740992.0), std::string{"9007199254740992"});
                                  requireEqual(state::detail::number(1.5), std::string{"1.5"});
                                  requireEqual(state::detail::number(std::numeric_limits<double>::infinity()), std::string{"null"});
                              });
                          }};
