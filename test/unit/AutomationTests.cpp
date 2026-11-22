#include "TemporaryDirectory.hpp"
#include "Bdd.hpp"
#include <webfront/automation/Binding.hpp>
#include <webfront/automation/Mcp.hpp>
#include <webfront/automation/PageAuthentication.hpp>

using namespace webfront::bdd;

namespace {
struct BindingApp {
    struct UI {
        int                                       value;
        std::shared_ptr<std::vector<std::string>> scripts = std::make_shared<std::vector<std::string>>();
        int                                       id() const {
            return value;
        }
        void addScript(std::string text) const {
            scripts->push_back(std::move(text));
        }
        friend bool operator==(const UI& a, const UI& b) {
            return a.value == b.value;
        }
    };
    std::function<std::string(UI, std::string)> claim;
    std::function<void(UI)>                     started, closed;
    template <typename Handler>
    void cppFunction(std::string_view, Handler handler) {
        if constexpr (std::is_invocable_r_v<std::string, Handler, UI, std::string>)
            claim = std::move(handler);
    }
    webfront::UISubscription subscribeUIStarted(std::function<void(UI)> handler) {
        started = std::move(handler);
        return {};
    }
    webfront::UISubscription subscribeUIClosed(std::function<void(UI)> handler) {
        closed = std::move(handler);
        return {};
    }
};
using namespace webfront::automation;
Object command(const Session& session, std::string operation, std::string args = "{}") {
    return Object(objectJson({
        {  "session", webfront::json::quote(Object(session.describe()).string("session"))},
        {"operation",                                    webfront::json::quote(operation)},
        {"arguments",                                                                args}
    }));
}

const Scenario earlierBrowser{"An earlier browser cannot claim the launched automation window", [] {
    webfront::test::TemporaryDirectory directory;
    auto                               session = std::make_shared<Session>(SessionOptions{directory.path(), {"build"}});
    BindingApp                         app;
    Binding                            binding{app, session};
    const auto                         page  = binding.launchPage();
    const auto                         token = page.substr(page.find('=') + 1);
    require(token.size() == 64);
    BindingApp::UI earlier{1}, launched{2};
    app.started(earlier);
    require(earlier.scripts->size() == 1);
    require(earlier.scripts->front().find(token) == std::string::npos);
    require(earlier.scripts->front().find("importKey") == std::string::npos);
    requireThrows([&] { app.claim(earlier, std::string(64, '0')); });
    app.closed(earlier);
    require(!session->isClosed());
    app.started(launched);
    requireNothrow([&] { app.claim(launched, token); });
    require(launched.scripts->size() == 2);
    require(launched.scripts->back().find("importKey") != std::string::npos);
    requireThrows([&] { app.claim(earlier, token); });
    requireThrows([&] { app.claim(launched, token); });
    app.closed(earlier);
    require(!session->isClosed());
    app.closed(launched);
    require(session->isClosed());
}};
Object query(const Session& session, std::string id) {
    return Object(objectJson({
        {"session", webfront::json::quote(Object(session.describe()).string("session"))},
        {  "runId",                                           webfront::json::quote(id)}
    }));
}
}  // namespace

const Scenario pageCallback{"Page callback authentication matches RFC 4231 and refuses wire replay or forgery", [] {
    using namespace webfront::automation::authentication;
    require(hmac(std::string(20, '\x0b'), "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    require(hmac("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    require(hmac(std::string(131, static_cast<char>(0xaa)), "Test Using Larger Than Block-Size Key - Hash Key First")
            == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    const std::string  key(64, 'a');
    PageAuthentication auth{key};
    const auto         captured = hmac(key, payload("1", "poll", {"latest output é😀"}));
    requireThrows([&] { auth.authorize("1", captured, "failed", {"forged by a frame"}); });
    requireThrows([&] { auth.authorize("1", captured, "poll", {"tampered output"}); });
    requireNothrow([&] { auth.authorize("1", captured, "poll", {"latest output é😀"}); });
    requireThrows([&] { auth.authorize("1", captured, "poll", {"latest output é😀"}); });
    const auto completed = hmac(key, payload("2", "complete", {"run-1", R"({"exitCode":0})"}));
    requireThrows([&] { auth.authorize("2", completed, "complete", {"run-2", R"({"exitCode":0})"}); });
    requireNothrow([&] { auth.authorize("2", completed, "complete", {"run-1", R"({"exitCode":0})"}); });
    requireThrows([&] { auth.authorize("3", captured, "poll", {"latest output é😀"}); });
    requireNothrow([&] { auth.authorize("5", hmac(key, payload("5", "poll", {"after dropped messages"})), "poll", {"after dropped messages"}); });
    requireThrows([&] { auth.authorize("4", hmac(key, payload("4", "poll", {"late"})), "poll", {"late"}); });
    requireNothrow([&] { auth.authorize("8", hmac(key, payload("8", "failed", {"transport failure"})), "failed", {"transport failure"}); });
    require(hmac(key, payload("3", "complete", {"a", "bc"})) != hmac(key, payload("3", "complete", {"ab", "c"})));
    require(hmac(key, payload("3", "reply:poll", {"null"})) != hmac(key, payload("3", "poll", {"null"})));
}};

const Scenario automationAuthorization{"Automation authorization stays native and binds requests to a project session", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build", "test", "inspect"}}
    };
    Session other{
        {directory.path(), {"build"}}
    };
    requireThrows([&] { session.start(command(session, "build")); });
    session.poll();
    given("a ready session and an authorized operation", [&] {
        const auto id = Object(session.start(command(session, "build", R"({"entry":"main"})"))).string("runId");
        require(Object(session.poll()).string("operation") == "build");
        requireThrows([&] { session.start(command(session, "test")); });
        requireThrows([&] { session.complete("wrong", R"({"exitCode":0,"output":"","report":null})"); });
        session.complete(id, R"({"exitCode":1,"output":"build error","diagnostics":[{"file":"main.ts","line":2,"text":"failure"}],"report":null})");
        const Object result{session.result(query(session, id))};
        require(result.string("state") == "failed");
        require(Object(result.raw("result")).raw("diagnostics").find("main.ts") != std::string::npos);
    });
    given("operations and arguments outside the native grant", [&] {
        for (const auto operation : {"run", "exec", "install", "restore", "shell", "eval", "preview", "fill"})
            requireThrows([&] { session.start(command(session, operation)); });
        requireThrows([&] { session.start(command(other, "build")); });
        requireThrows([&] { session.start(command(session, "build", R"({"entry":"../escape"})")); });
        requireThrows([&] { session.start(command(session, "build", R"({"entry":"--help"})")); });
        requireThrows([&] { session.start(command(session, "build", R"({"allow":["exec"]})")); });
        requireThrows([&] { session.start(command(session, "test", R"({"pattern":"--timeout"})")); });
        require(Object(session.describe()).raw("audit").find("denied") != std::string::npos);
    });
}};

const Scenario automationEntries{"Automation entries accept manifest labels while refusing unsafe paths", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build", "preview"}}
    };
    session.poll();
    given("entry labels and relative paths accepted by wf", [&] {
        for (const auto operation : {"build", "preview"}) {
            for (const auto entry : {"main", "app:debug", "app..debug", "src/main.tsx"}) {
                const auto   arguments = objectJson({
                    {"entry", webfront::json::quote(entry)}
                });
                const auto   id        = Object(session.start(command(session, operation, arguments))).string("runId");
                const Object dispatched{session.poll()};
                require(Object(dispatched.raw("arguments")).string("entry") == entry);
                session.complete(id, R"({"exitCode":0,"output":"","report":null})");
            }
        }
    });
    given("paths that cannot name manifest entries", [&] {
        for (const auto entry : {"../escape", "/absolute", "--help", "C:/escape", "src/../escape", "src\\main.ts", "src/app:debug.ts"}) {
            const auto arguments = objectJson({
                {"entry", webfront::json::quote(entry)}
            });
            requireThrows([&] { session.start(command(session, "build", arguments)); });
        }
    });
}};

const Scenario projectSetup{"Project setup operations need their own grants and plain arguments", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            denied{
        {directory.path(), {"build"}}
    };
    Session session{
        {directory.path(), {"init", "create", "install", "restore", "embed"}}
    };
    denied.poll();
    session.poll();
    const auto accepted = [&](const char* operation, const char* arguments) {
        const auto id = Object(session.start(command(session, operation, arguments))).string("runId");
        session.poll();
        session.complete(id, R"({"exitCode":0,"output":"","report":null})");
    };
    given("grants that older environments cannot run", [&] {
        require(denied.versionedOperations().empty());
        const auto versioned = session.versionedOperations();
        require(versioned.size() == 5);
        for (const auto& [operation, since] : versioned)
            require(since == "0.6.4");
        require(webfront::automation::connectionScript("secret", versioned).find("[\"embed\",\"0.6.4\"]") != std::string::npos);
    });
    given("a session without those grants", [&] {
        for (const auto operation : {"init", "create", "install", "restore", "embed"})
            requireThrows([&] { denied.start(command(denied, operation, operation == std::string_view("create") ? R"({"starter":"life"})" : "{}")); });
    });
    given("granted operations with valid arguments", [&] {
        accepted("init", R"({"name":"demo"})");
        accepted("create", R"({"starter":"life","directory":"ui"})");
        accepted("install", "{}");
        accepted("install", R"({"package":"react@19.3.1"})");
        accepted("restore", "{}");
        accepted("embed", R"({"provider":"LifeUI"})");
        accepted("embed", R"({"entry":"ui/main.tsx","output":"generated/LifeUI.hpp"})");
    });
    given("options, escapes and incomplete requests", [&] {
        requireThrows([&] { session.start(command(session, "create", "{}")); });
        requireThrows([&] { session.start(command(session, "create", R"({"starter":"--help"})")); });
        requireThrows([&] { session.start(command(session, "create", R"({"starter":"life","directory":"../escape"})")); });
        requireThrows([&] { session.start(command(session, "create", R"({"starter":"life","directory":"/absolute"})")); });
        requireThrows([&] { session.start(command(session, "install", R"({"package":"--update"})")); });
        requireThrows([&] { session.start(command(session, "install", R"({"package":"a b"})")); });
        requireThrows([&] { session.start(command(session, "init", R"({"name":"-x"})")); });
        requireThrows([&] { session.start(command(session, "restore", R"({"offline":"true"})")); });
        requireThrows([&] { session.start(command(session, "embed", "{}")); });
        requireThrows([&] { session.start(command(session, "embed", R"({"entry":"ui/main.tsx"})")); });
        requireThrows([&] { session.start(command(session, "embed", R"({"provider":"LifeUI","output":"a.hpp"})")); });
        requireThrows([&] { session.start(command(session, "embed", R"({"entry":"ui/main.tsx","output":"../a.hpp"})")); });
    });
}};

const Scenario authorizationPrecedes{"Authorization precedes operation and argument validation", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}}
    };
    const auto identity = Object(session.describe()).string("session");
    require(identity.size() == 64);
    Session other{
        {directory.path(), {"build"}}
    };
    require(Object(other.describe()).string("session") != identity);
    const auto refuses = [&](const Object& request, std::string_view code) {
        try {
            session.start(request);
            fail("Unauthorized request was accepted");
        } catch (const OperationError& error) {
            require(error.code == code);
        }
    };
    for (const auto operation : {"build", "exec", "unknown"}) {
        refuses(Object(objectJson({
                    {  "session",                      "\"wrong\""},
                    {"operation", webfront::json::quote(operation)},
                    {"arguments",                             "17"}
        })),
                "wrong_session");
    }
    refuses(Object(R"({"session":17,"operation":false,"arguments":17})"), "wrong_session");
    refuses(command(session, "test", "17"), "not_authorized");
    const auto audit = Object(session.describe()).raw("audit");
    require(audit.find("denied") != std::string::npos);
    require(audit.find("unknown") != std::string::npos);
    require(audit.find("test") != std::string::npos);
}};

const Scenario exitCode130{"Exit code 130 needs an actual cancellation request", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}}
    };
    session.poll();
    const auto first = Object(session.start(command(session, "build"))).string("runId");
    session.poll();
    session.complete(first, R"({"exitCode":130,"output":"script exit","report":null})");
    require(Object(session.result(query(session, first))).string("state") == "failed");
    const auto second = Object(session.start(command(session, "build"))).string("runId");
    session.poll();
    session.complete(second, R"({"exitCode":130,"output":"human stop","report":null,"cancelled":true})");
    require(Object(session.result(query(session, second))).string("state") == "cancelled");
}};

const Scenario automationCancellation{"Automation cancellation and native deadlines cannot reuse a stalled renderer", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}, std::chrono::seconds{1}}
    };
    session.poll();
    const auto id = Object(session.start(command(session, "build"))).string("runId");
    given("cancellation before dispatch", [&] {
        session.cancel(query(session, id));
        require(session.poll() == "null");
        require(Object(session.result(query(session, id))).string("state") == "cancelled");
        requireNothrow([&] { session.start(command(session, "build")); });
    });
    given("cancellation after dispatch", [&] {
        session.poll();
        session.cancel(query(session, id));
        require(Object(session.poll()).string("cancel") == id);
        session.complete(id, R"({"exitCode":130,"output":"stopped","report":null})");
        require(Object(session.result(query(session, id))).string("state") == "cancelled");
    });
    given("a native deadline, independent of JS callbacks", [&] {
        require(!session.expire());
        require(session.expire(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        require(session.isClosed());
        require(Object(session.result(query(session, id))).string("state") == "timeout");
        requireThrows([&] { session.complete(id, R"({"exitCode":0,"output":"","report":null})"); });
        requireThrows([&] { session.start(command(session, "build")); });
    });
}};

const Scenario automationStartup{"Automation startup expiry respects readiness at the deadline", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}}
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    require(!session.expireStartup(deadline, deadline - std::chrono::milliseconds{1}));
    given("the renderer has polled before expiry is checked", [&] {
        session.poll();
        require(!session.expireStartup(deadline, deadline));
        require(!session.isClosed());
    });
    given("the renderer has not polled at the deadline", [&] {
        require(session.expireStartup(deadline, deadline));
        require(session.isClosed());
        requireThrows([&] { session.start(command(session, "build")); });
        require(!session.expireStartup(deadline, deadline));
    });
}};

const Scenario mCPInitialization{"MCP initialization, tool errors and request identifiers follow the supported profile", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {}}
    };
    Mcp mcp{session};
    require(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")).has("error"));
    const Object initialized{*mcp.receive(
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"1"},"capabilities":{}}})")};
    require(initialized.string("id") == "init");
    require(Object(initialized.raw("result")).string("protocolVersion") == "2025-11-25");
    require(!mcp.receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    require(mcp.receive(R"({"jsonrpc":"2.0","id":41,"method":"tools/list"})")->find('\n') == std::string::npos);
    require(Object(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":42,"method":"tools/list"})")).raw("result")).raw("tools").find("webfront.start")
            != std::string::npos);
    const Object tool{
        Object(*mcp.receive(
                   R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"webfront.start","arguments":{"session":"wrong","operation":"exec"}}})"))
            .raw("result")};
    require(tool.raw("isError") == "true");
    require(Object(*mcp.receive(R"({"jsonrpc":"2.0","id":null,"method":"ping"})")).has("error"));
    for (const auto id : {"1.0", "1e2", "-0.25", "123456789012345678901234567890"}) {
        const Object reply{*mcp.receive(std::string{"{\"jsonrpc\":\"2.0\",\"id\":"} + id + ",\"method\":\"ping\"}")};
        require(!reply.has("error"));
        require(reply.raw("id") == id);
    }
    require(Object(*mcp.receive("{bad")).has("error"));
    require(!mcp.receive(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":42}})"));
    for (const auto notification : {R"({"jsonrpc":"1.0","method":"ping"})", R"({"jsonrpc":"2.0"})", R"({"method":"ping"})", R"({"jsonrpc":"2.0","method":17})"})
        require(!mcp.receive(notification));
    require(Object(*mcp.receive(R"({"jsonrpc":"1.0","id":7,"method":"ping"})")).has("error"));
}};

const Scenario automationAudit{"Automation audit retains authorization decisions through busy and malformed retries", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}}
    };
    requireThrows([&] { session.start(command(session, "build")); });
    require(Object(session.describe()).raw("audit") == "[]");
    session.poll();
    requireThrows([&] { session.start(command(session, "test")); });
    const auto denied = Object(session.describe()).raw("audit");
    require(denied.find("\"operation\":\"test\"") != std::string::npos);
    const auto id    = Object(session.start(command(session, "build"))).string("runId");
    const auto audit = Object(session.describe()).raw("audit");
    require(audit.find(id) != std::string::npos);
    for (int i = 0; i < 100; ++i) {
        requireThrows([&] { session.start(command(session, "build")); });
        requireThrows([&] { session.start(command(session, "build", R"({"entry":"../escape"})")); });
    }
    require(Object(session.describe()).raw("audit") == audit);
    session.poll();
    session.complete(id, R"({"exitCode":0,"output":"","report":null})");
    require(Object(session.describe()).raw("audit") == audit);
}};

const Scenario automationJSON{"Automation JSON rejects duplicate keys, non-UTF8 and deeply nested or oversized input", [] {
    requireThrows([&] { Object(R"({"operation":"build","operation":"exec"})"); });
    requireThrows([&] { Object(std::string(60001, ' ')); });
    requireThrows([&] { Object(std::string("{\"x\":\"") + std::string("\xC0\xAF", 2) + "\"}"); });
    requireThrows([&] { Object(std::string("{\"x\":") + std::string(33, '[') + "0" + std::string(33, ']') + "}"); });
    requireNothrow([&] { Object(R"({"x":"é😀","nested":[{"number":-1.25e+2}]})"); });
    requireThrows([&] { Object("{} trailing"); });
}};

const Scenario versionedOperation{"The versioned operation schema projects the same fields and permissions to native and MCP clients", [] {
    using namespace webfront::automation;
    const Object contract{contractSource};
    const Object properties{contract.raw("properties")};
    const auto   names = contractStrings(Object(properties.raw("operation")).raw("enum"));
    const Object definitions{contract.raw("x-webfront-operations")};
    require(names.size() == definitions.members().size());
    const Object arguments{Object(properties.raw("arguments")).raw("properties")};
    for (const auto& name : names) {
        const auto definition = operationContract(name);
        require(!definition.string("permission").empty());
        const auto allowed = contractStrings(definition.raw("arguments"));
        for (const auto& field : allowed)
            require(arguments.has(field));
        for (const auto& field : contractStrings(definition.raw("required")))
            require(std::ranges::find(allowed, field) != allowed.end());
    }
    require(Object(startSchema()).has("properties"));
    require(&contractObject() == &contractObject());
    require(&operationContract("build") == &operationContract("build"));
    require(&startSchema() == &startSchema());
}};

const Scenario completedAutomation{"A completed automation snapshot replaces progress with the final bounded output", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}}
    };
    session.poll();
    const auto id = Object(session.start(command(session, "build"))).string("runId");
    session.poll("last progress tail");
    const auto output = std::string(12000, 'x') + "final summary";
    session.complete(id,
                     objectJson({
                         {"exitCode",                           "0"},
                         {  "output", webfront::json::quote(output)},
                         {  "report",                        "null"}
    }));
    const Object result{session.result(query(session, id))};
    require(result.string("output") == output);
    require(result.string("output") == Object(result.raw("result")).string("output"));

    const auto next = Object(session.start(command(session, "build"))).string("runId");
    require(Object(session.result(query(session, next))).string("output").empty());
    require(Object(session.poll("previous runner output")).string("runId") == next);
    require(Object(session.result(query(session, next))).string("output").empty());
    session.poll("new run progress");
    require(Object(session.result(query(session, next))).string("output") == "new run progress");
}};

const Scenario completionNative{"A completion the native side cannot validate fails its run instead of wedging the session", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {directory.path(), {"build"}}
    };
    session.poll();
    given("a running operation", [&] {
        const auto id = Object(session.start(command(session, "build"))).string("runId");
        require(Object(session.poll()).string("operation") == "build");
        when("the page reports a lone surrogate, which no bridge string can carry", [&] {
            requireNothrow([&] { session.complete(id, "{\"exitCode\":0,\"output\":\"cut \\ud83d\"}"); });
            then("the run fails with the reason and the session stays usable", [&] {
                const Object result{session.result(query(session, id))};
                require(result.string("state") == "failed");
                require(Object(result.raw("result")).string("output").find("Invalid automation completion") != std::string::npos);
                require(!session.expire(std::chrono::steady_clock::now() + std::chrono::hours{1}));
                require(!session.isClosed());
                requireNothrow([&] { session.start(command(session, "build")); });
            });
        });
        when("the page reports an exit code out of range", [&] {
            session.complete(id, R"({"exitCode":999,"output":""})");
            then("the run fails as well", [&] {
                require(Object(session.result(query(session, id))).string("state") == "failed");
            });
        });
        when("an invalid completion has a very long unexpected member name", [&] {
            session.complete(id,
                             objectJson({
                                 {std::string(50000, 'x'), "0"}
            }));
            const Object result{session.result(query(session, id))};
            require(result.string("state") == "failed");
            require(result.string("output").size() < 1200);
            require(result.string("output") == Object(result.raw("result")).string("output"));
            requireNothrow([&] { session.start(command(session, "build")); });
        });
    });
}};

const Scenario mCPErrors{"MCP errors keep the request id and distinguish a lost transport from bad arguments", [] {
    struct LostBackend {
        std::string describe() const {
            throw TransportError("Native attachment is unavailable");
        }
        std::string start(const Object&) const {
            throw TransportError("Native attachment receive deadline exceeded");
        }
        std::string result(const Object&) const {
            return "{}";
        }
        std::string cancel(const Object&) const {
            return "{}";
        }
    } backend;
    Mcp mcp{backend};
    mcp.receive(
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"1"},"capabilities":{}}})");
    mcp.receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    given("a request with a valid id but a wrong version or no method", [&] {
        const Object wrongVersion{*mcp.receive(R"({"jsonrpc":"1.0","id":7,"method":"ping"})")};
        then("the error answers that id with the invalid-request code", [&] {
            require(wrongVersion.raw("id") == "7");
            require(Object(wrongVersion.raw("error")).raw("code") == "-32600");
            const Object noMethod{*mcp.receive(R"({"jsonrpc":"2.0","id":8})")};
            require(noMethod.raw("id") == "8");
            require(Object(noMethod.raw("error")).raw("code") == "-32600");
            const Object extra{*mcp.receive(R"({"jsonrpc":"2.0","id":9,"method":"ping","other":1})")};
            require(extra.raw("id") == "9");
            require(Object(extra.raw("error")).raw("code") == "-32600");
        });
        then("only unparseable input is a parse error, without an id", [&] {
            const Object broken{*mcp.receive("{bad")};
            require(broken.raw("id") == "null");
            require(Object(broken.raw("error")).raw("code") == "-32700");
        });
    });
    given("a backend whose channel to the owner failed", [&] {
        const Object tool{
            Object(*mcp.receive(
                       R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"webfront.start","arguments":{"session":"s","operation":"build"}}})"))
                .raw("result")};
        then("the tool reports disconnected, not invalid arguments, and says the outcome is unknown", [&] {
            require(tool.raw("isError") == "true");
            const Object content{tool.raw("structuredContent")};
            require(content.string("code") == "disconnected");
            require(content.string("error").find("outcome") != std::string::npos);
        });
    });
}};

const Scenario applicationCallback{"An application callback is called only when its name was allow-listed with the call grant", [] {
    webfront::test::TemporaryDirectory directory;
    Session                            session{
        {.project = directory.path(), .allow = {"call"}, .callbacks = {"app.render", "app.count"}}
    };
    Session withoutList{
        {directory.path(), {"call"}}
    };
    Session withoutGrant{
        {directory.path(), {"check"}}
    };
    session.poll();
    withoutList.poll();
    withoutGrant.poll();
    given("the names the owner allow-listed", [&] {
        const Object described{session.describe()};
        require(described.raw("callbacks") == R"(["app.render","app.count"])");
        require(Object(withoutList.describe()).raw("callbacks") == "[]");
        const auto versioned = session.versionedOperations();
        require(versioned.size() == 1);
        require(versioned[0].first == "call");
        require(versioned[0].second == "0.6.6");
        then("a call to one of them, with or without a string argument, is accepted", [&] {
            const auto id = Object(session.start(command(session, "call", R"({"callback":"app.render","value":"text"})"))).string("runId");
            require(Object(session.poll()).string("operation") == "call");
            session.complete(id, R"({"exitCode":0,"output":"","report":null,"data":{"callback":"app.render","result":"rendered:text"}})");
            const auto next = Object(session.start(command(session, "call", R"({"callback":"app.count"})"))).string("runId");
            session.poll();
            session.complete(next, R"({"exitCode":0,"output":"","report":null})");
        });
    });
    given("a callback that was not allow-listed", [&] {
        then("it is refused and audited, whatever its name", [&] {
            for (const auto* name : {"app.secret", "app.render.extra", "app", "APP.RENDER"})
                requireThrows<webfront::automation::OperationError>([&] { session.start(command(session, "call", std::string(R"({"callback":")") + name + "\"}")); });
            require(Object(session.describe()).raw("audit").find("\"decision\":\"denied\"") != std::string::npos);
        });
        then("a session without a list, or without the grant, refuses every call", [&] {
            requireThrows([&] { withoutList.start(command(withoutList, "call", R"({"callback":"app.render"})")); });
            requireThrows([&] { withoutGrant.start(command(withoutGrant, "call", R"({"callback":"app.render"})")); });
        });
    });
    given("names and arguments that could reach internals", [&] {
        then("webfront.* names, odd characters and extra arguments are refused before any allow-list lookup", [&] {
            const auto make = [&](std::vector<std::string> allow, std::vector<std::string> callbacks) {
                return Session{webfront::automation::SessionOptions{.project = directory.path(), .allow = std::move(allow), .callbacks = std::move(callbacks)}};
            };
            requireThrows([&] { make({"call"}, {"webfront.developer.read"}); });
            requireThrows([&] { make({"call"}, {"app render"}); });
            requireThrows([&] { make({"call"}, {""}); });
            requireThrows([&] { make({"check"}, {"app.render"}); });  // names need the grant
            requireThrows([&] { session.start(command(session, "call", "{}")); });
            requireThrows([&] { session.start(command(session, "call", R"({"callback":"app.render","args":"x"})")); });
            requireThrows([&] { session.start(command(session, "call", R"({"callback":"webfront.automation.poll"})")); });
        });
    });
}};
