#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/automation/Mcp.hpp>

namespace {
using namespace webfront::automation;
Object command(const Session& session, std::string operation, std::string args = "{}") {
    return Object(objectJson({
        {  "session", webfront::json::quote(Object(session.describe()).string("session"))},
        {"operation",                                    webfront::json::quote(operation)},
        {"arguments",                                                                args}
    }));
}
Object query(const Session& session, std::string id) {
    return Object(objectJson({
        {"session", webfront::json::quote(Object(session.describe()).string("session"))},
        {  "runId",                                           webfront::json::quote(id)}
    }));
}
}  // namespace

SCENARIO("Automation authorization stays native and binds requests to a project session") {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build", "test", "inspect"}}
    };
    Session other{
        {directory.path(), {"build"}}
    };
    REQUIRE_THROWS(session.start(command(session, "build")));
    session.poll();
    GIVEN("a ready session and an authorized operation") {
        const auto id = Object(session.start(command(session, "build", R"({"entry":"main"})"))).string("runId");
        REQUIRE(Object(session.poll()).string("operation") == "build");
        REQUIRE_THROWS(session.start(command(session, "test")));
        REQUIRE_THROWS(session.complete("wrong", R"({"exitCode":0,"output":"","report":null})"));
        session.complete(id, R"({"exitCode":1,"output":"build error","diagnostics":[{"file":"main.ts","line":2,"text":"failure"}],"report":null})");
        const Object result{session.result(query(session, id))};
        REQUIRE(result.string("state") == "failed");
        REQUIRE(Object(result.raw("result")).raw("diagnostics").find("main.ts") != std::string::npos);
    }
    GIVEN("operations and arguments outside the native grant") {
        for (const auto operation : {"run", "exec", "install", "restore", "shell", "eval", "preview", "fill"})
            REQUIRE_THROWS(session.start(command(session, operation)));
        REQUIRE_THROWS(session.start(command(other, "build")));
        REQUIRE_THROWS(session.start(command(session, "build", R"({"entry":"../escape"})")));
        REQUIRE_THROWS(session.start(command(session, "build", R"({"entry":"--help"})")));
        REQUIRE_THROWS(session.start(command(session, "build", R"({"allow":["exec"]})")));
        REQUIRE_THROWS(session.start(command(session, "test", R"({"pattern":"--timeout"})")));
        REQUIRE(Object(session.describe()).raw("audit").find("denied") != std::string::npos);
    }
}

SCENARIO("Automation cancellation and native deadlines cannot reuse a stalled renderer") {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}, std::chrono::seconds{1}}
    };
    session.poll();
    const auto id = Object(session.start(command(session, "build"))).string("runId");
    GIVEN("cancellation before dispatch") {
        session.cancel(query(session, id));
        REQUIRE(session.poll() == "null");
        REQUIRE(Object(session.result(query(session, id))).string("state") == "cancelled");
        REQUIRE_NOTHROW(session.start(command(session, "build")));
    }
    GIVEN("cancellation after dispatch") {
        session.poll();
        session.cancel(query(session, id));
        REQUIRE(Object(session.poll()).string("cancel") == id);
        session.complete(id, R"({"exitCode":130,"output":"stopped","report":null})");
        REQUIRE(Object(session.result(query(session, id))).string("state") == "cancelled");
    }
    GIVEN("a native deadline, independent of JS callbacks") {
        REQUIRE_FALSE(session.expire());
        REQUIRE(session.expire(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        REQUIRE(session.isClosed());
        REQUIRE(Object(session.result(query(session, id))).string("state") == "timeout");
        REQUIRE_THROWS(session.complete(id, R"({"exitCode":0,"output":"","report":null})"));
        REQUIRE_THROWS(session.start(command(session, "build")));
    }
}

SCENARIO("MCP initialization, tool errors and request identifiers follow the supported profile") {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {}}
    };
    Mcp mcp{session};
    REQUIRE(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")).has("error"));
    const Object initialized{*mcp.receive(
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"1"},"capabilities":{}}})")};
    REQUIRE(initialized.string("id") == "init");
    REQUIRE(Object(initialized.raw("result")).string("protocolVersion") == "2025-11-25");
    REQUIRE_FALSE(mcp.receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    REQUIRE(mcp.receive(R"({"jsonrpc":"2.0","id":41,"method":"tools/list"})")->find('\n') == std::string::npos);
    REQUIRE(Object(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":42,"method":"tools/list"})")).raw("result")).raw("tools").find("webfront.start")
            != std::string::npos);
    const Object tool{
        Object(*mcp.receive(
                   R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"webfront.start","arguments":{"session":"wrong","operation":"exec"}}})"))
            .raw("result")};
    REQUIRE(tool.raw("isError") == "true");
    REQUIRE(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":null,"method":"ping"})")).has("error"));
    REQUIRE(Object(*mcp.receive("{bad")).has("error"));
    REQUIRE_FALSE(mcp.receive(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":42}})"));
}

SCENARIO("Automation JSON rejects duplicate keys, non-UTF8 and deeply nested or oversized input") {
    REQUIRE_THROWS(Object(R"({"operation":"build","operation":"exec"})"));
    REQUIRE_THROWS(Object(std::string(60001, ' ')));
    REQUIRE_THROWS(Object(std::string("{\"x\":\"") + std::string("\xC0\xAF", 2) + "\"}"));
    REQUIRE_THROWS(Object(std::string("{\"x\":") + std::string(33, '[') + "0" + std::string(33, ']') + "}"));
    REQUIRE_NOTHROW(Object(R"({"x":"é😀","nested":[{"number":-1.25e+2}]})"));
    REQUIRE_THROWS(Object("{} trailing"));
}

SCENARIO("The versioned operation schema projects the same fields and permissions to native and MCP clients") {
    using namespace webfront::automation;
    const Object contract{contractSource};
    const Object properties{contract.raw("properties")};
    const auto   names = contractStrings(Object(properties.raw("operation")).raw("enum"));
    const Object definitions{contract.raw("x-webfront-operations")};
    REQUIRE(names.size() == definitions.members().size());
    const Object arguments{Object(properties.raw("arguments")).raw("properties")};
    for (const auto& name : names) {
        const auto definition = operationContract(name);
        REQUIRE_FALSE(definition.string("permission").empty());
        const auto allowed = contractStrings(definition.raw("arguments"));
        for (const auto& field : allowed)
            REQUIRE(arguments.has(field));
        for (const auto& field : contractStrings(definition.raw("required")))
            REQUIRE(std::ranges::find(allowed, field) != allowed.end());
    }
    REQUIRE(Object(startSchema()).has("properties"));
}
