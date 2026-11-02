#include "../driver/TestFrontend.hpp"
#include <webfront/WebFront.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

void checkWKWebViewHeader();

namespace {
using namespace webfront::frontend::detail::wkwebview;
using Frontend = webfront::testdriver::QuietFrontend<webfront::frontend::WKWebViewFrontend>;
using App      = webfront::WebFrontWithFrontend<Frontend>;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Function>
void expectFailure(Function function, std::string_view diagnostic) {
    try {
        function();
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).contains(diagnostic), error.what());
        return;
    }
    throw std::runtime_error("Expected WKWebView failure");
}

void repeatedShow() {
    for (int iteration = 0; iteration != 2; ++iteration) {
        for (auto page : {"no-bridge.html", "no-bridge.html?early-close", "rejected-ready.html", "encoded path %23.html?x=1#example"}) {
            App app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
            app.show(page);
        }
        App app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
        expectFailure(
            [&] {
                app.show("missing-wkwebview-page.html");
            },
            "HTTP 404");
    }
    App app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("no-bridge.html");
}

void mainThread() {
    // initialize() also has to reject a worker after successful main-thread initialization.
    Frontend::initialize();
    auto worker = std::async(std::launch::async, [] {
        expectFailure(
            [] {
                Frontend::initialize();
            },
            "main thread");
        expectFailure(
            [] {
                Frontend::open("0", "index.html");
            },
            "main thread");
    });
    worker.get();
}

void nativeClose(bool quit) {
    Object     pool(send(type("NSAutoreleasePool"), "new"));
    const auto app              = send(type("NSApplication"), "sharedApplication");
    const auto previousMenu     = send(app, "mainMenu");
    const auto previousDelegate = send(app, "delegate");
    {
        Session session("http://127.0.0.1:0/", {.visible = webfront::testdriver::diagnosticWindows()});
        session.create();
        require(static_cast<bool>(send<BOOL>(session.window, "isVisible")) == webfront::testdriver::diagnosticWindows(),
                "Window visibility option was ignored");
        if (!webfront::testdriver::diagnosticWindows())
            require(send<long>(send(send(session.view, "configuration"), "preferences"), "inactiveSchedulingPolicy") == 2L,
                    "Hidden web content would suspend before asynchronous work finishes");
        // Exercise the real native entry points before any WebKit/bridge callback.
        if (quit)
            send<void>(app, "terminate:", static_cast<id>(nullptr));
        else
            send<void>(session.window, "performClose:", static_cast<id>(nullptr));
        require(session.closed, "Native close/Quit did not close the session");
        session.run();
    }
    const auto restoredMenu = send(app, "mainMenu");
    // AppKit materializes an empty NSMenu when restoring a previously nil mainMenu.
    require(send(app, "delegate") == previousDelegate && (restoredMenu == previousMenu || (!previousMenu && send<long>(restoredMenu, "numberOfItems") == 0)),
            "Session did not restore the application delegate/menu");
    // The process is still alive and a new AppKit run is possible.
    App next(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    next.show("no-bridge.html");
}

struct TestSocket {
    explicit TestSocket(int descriptor) : descriptor(descriptor) {}
    TestSocket(const TestSocket&)            = delete;
    TestSocket& operator=(const TestSocket&) = delete;
    ~TestSocket() {
        ::close(descriptor);
    }
    int descriptor;
};

struct Listener {
    Listener() : socket(::socket(AF_INET, SOCK_STREAM, 0)) {
        require(socket.descriptor >= 0, "Cannot create HTTP listener");
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket.descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(socket.descriptor, 4) == 0,
                "Cannot bind HTTP listener");
        socklen_t size = sizeof(address);
        require(getsockname(socket.descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Cannot find HTTP port");
        port = std::to_string(ntohs(address.sin_port));
    }
    TestSocket  socket;
    std::string port;
};

void startupTimeout() {
    // No accept/response: navigation stalls without relying on public networking.
    Listener                             listener;
    webfront::frontend::WKWebViewOptions options{.startupTimeout = std::chrono::milliseconds(200), .visible = webfront::testdriver::diagnosticWindows()};
    expectFailure(
        [&] {
            Frontend::open("127.0.0.1", listener.port, "index.html", options);
        },
        "timed out");
    expectFailure(
        [] {
            Frontend::open("127.0.0.1", "0", "index.html", webfront::frontend::WKWebViewOptions{.startupTimeout = std::chrono::milliseconds(0)});
        },
        "positive");
}

std::string redirectResponse(std::string_view request, const std::string& port) {
    if (request.starts_with("GET /redirect "))
        return "HTTP/1.1 302 Found\r\nLocation: http://localhost:" + port + "/finish\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const std::string body = "<script>window.close()</script>";
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

void sendResponse(int client, std::string_view response) {
    std::size_t offset = 0;
    while (offset < response.size()) {
        const auto sent = ::send(client, response.data() + offset, response.size() - offset, 0);
        if (sent <= 0)
            return;
        offset += static_cast<std::size_t>(sent);
    }
}

void serveRedirectClient(int client, const std::string& port) {
    int noSignal = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
    const timeval receiveTimeout{1, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
    char       request[4096];
    const auto count = recv(client, request, sizeof(request), 0);
    if (count > 0)
        sendResponse(client, redirectResponse(std::string_view(request, static_cast<std::size_t>(count)), port));
}

void serveRedirects(const Listener& listener, std::stop_token stop) {
    while (!stop.stop_requested()) {
        pollfd ready{listener.socket.descriptor, POLLIN, 0};
        if (poll(&ready, 1, 50) <= 0)
            continue;
        TestSocket connection(accept(listener.socket.descriptor, nullptr, nullptr));
        if (connection.descriptor < 0)
            return;
        serveRedirectClient(connection.descriptor, listener.port);
    }
}

void redirectPolicy() {
    Listener     listener;
    std::jthread server([&](std::stop_token stop) {
        serveRedirects(listener, stop);
    });
    expectFailure(
        [&] {
            Frontend::open("127.0.0.1", listener.port, "redirect");
        },
        "contentPolicy");
    webfront::ContentPolicy allowed;
    allowed.navigationAllowedOrigins = {"http://localhost:" + listener.port};
    Frontend::open("127.0.0.1", listener.port, "redirect", allowed);
}

void settings() {
    Object pool(send(type("NSAutoreleasePool"), "new"));
    for (const bool enabled : {false, true}) {
        Session session("http://127.0.0.1:9002/",
                        {
                            .contentPolicy = {.devToolsEnabled = enabled, .contextMenusEnabled = enabled},
                            .visible       = webfront::testdriver::diagnosticWindows()
        });
        session.create();
        require(static_cast<bool>(send<BOOL>(session.window, "isVisible")) == webfront::testdriver::diagnosticWindows(),
                "Window visibility option was ignored");
        if (!webfront::testdriver::diagnosticWindows())
            require(send<long>(send(send(session.view, "configuration"), "preferences"), "inactiveSchedulingPolicy") == 2L,
                    "Hidden web content would suspend before asynchronous work finishes");
        require(static_cast<bool>(send<BOOL>(session.view, "isInspectable")) == enabled, "Inspector policy was ignored");
        require(!send<BOOL>(send(send(session.view, "configuration"), "websiteDataStore"), "isPersistent"), "Website data was persistent");
        Object menu(send(type("NSMenu"), "new"));
        session.addMenuItem(menu.get(), {"Copy", "copy:", "c"});
        send<void>(session.view, "willOpenMenu:withEvent:", menu.get(), static_cast<id>(nullptr));
        require((send<long>(menu.get(), "numberOfItems") != 0) == enabled, "Context-menu policy was ignored");
        require(send<long>(send(session.app, "mainMenu"), "numberOfItems") >= 2, "Missing application/Edit menus");
        require(utf8(send(send(send(session.app, "mainMenu"), "itemAtIndex:", 1L), "title")) == "Edit", "Edit menu has no visible title");
    }
}
void missingResponse() {
    Object  pool(send(type("NSAutoreleasePool"), "new"));
    Session session("http://127.0.0.1:0/", {.visible = webfront::testdriver::diagnosticWindows()});
    session.create();
    send<void>(session.delegate, "webView:didFinishNavigation:", session.view, static_cast<id>(nullptr));
    require(session.closed && !session.loaded, "An internal error document was treated as a successful HTTP load");
    expectFailure(
        [&] {
            session.run();
        },
        "without an HTTP response");
}

void committedNavigation() {
    Object  pool(send(type("NSAutoreleasePool"), "new"));
    Session session("http://127.0.0.1:9002/", {.visible = webfront::testdriver::diagnosticWindows()});
    session.create();
    bool notified               = false;
    session.navigationCancelled = [&] {
        notified = true;
    };
    // The document is interactive at commit, while a slow subresource may delay didFinish.
    send<void>(session.delegate, "webView:didCommitNavigation:", session.view, static_cast<id>(nullptr));
    session.rejectNavigation("initial navigation was denied by contentPolicy");
    send<void>(session.delegate, "webView:didFailNavigation:withError:", session.view, static_cast<id>(nullptr), static_cast<id>(nullptr));
    require(notified && !session.loaded && !session.closed && !session.error, "A denied later navigation closed the committed document");
    Session initial("http://127.0.0.1:9002/", {.visible = false});
    initial.rejectNavigation("initial navigation was denied by contentPolicy");
    require(initial.closed && initial.error, "A denied initial navigation was not reported");
}

void agentApplication() {
    Object     pool(send(type("NSAutoreleasePool"), "new"));
    const auto bundle = send(type("NSBundle"), "mainBundle");
    require(utf8(send(bundle, "bundleIdentifier")) == "org.webfront.tests.wkwebview-lifecycle", "Missing test application identity");
    Object key(text("LSUIElement"));
    require(send<BOOL>(send(bundle, "objectForInfoDictionaryKey:", key.get()), "boolValue"), "Test bundle is not an agent");
    const auto app            = send(type("NSApplication"), "sharedApplication");
    const auto previousPolicy = send<long>(app, "activationPolicy");
    {
        Session session("http://127.0.0.1:0/");
        session.create();
        require(send<long>(app, "activationPolicy") == 1L, "Agent session enabled a Dock icon");
        require(send<BOOL>(session.window, "isVisible"), "Agent session did not create a visible window");
    }
    require(send<long>(app, "activationPolicy") == previousPolicy, "Session did not restore the host activation policy");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

/// Opens a page with the file-service test injections of WKWebViewOptions, which a scenario fills in before show().
struct ScenarioFrontend : Frontend {
    static inline std::function<void(webfront::frontend::WKWebViewOptions&)> configure;
    static inline std::function<void(Session&)>                             started;

    static void open(std::string_view port, std::string_view page) {
        initialize();
        Object                               pool(send(type("NSAutoreleasePool"), "new"));
        webfront::frontend::WKWebViewOptions options{.visible = webfront::testdriver::diagnosticWindows()};
        if (configure)
            configure(options);
        Session session(webfront::frontend::detail::applicationUrl("127.0.0.1", port, page), options);
        session.create();
        session.start();
        if (started)
            started(session);
        session.run();
    }
};
using ScenarioApp = webfront::WebFrontWithFrontend<ScenarioFrontend>;

/// Ends the AppKit loop from a native callback, as the Quit menu item does.
void quitApplication() {
    send<void>(send(type("NSApplication"), "sharedApplication"), "terminate:", static_cast<id>(nullptr));
}

void resetScenario() {
    ScenarioFrontend::configure = nullptr;
    ScenarioFrontend::started   = nullptr;
}

void download() {
    const auto folder = std::filesystem::temp_directory_path() / ("webfront-wk-download-" + std::to_string(getpid()));
    std::filesystem::remove_all(folder);
    std::string destination;
    bool        done        = false;
    ScenarioFrontend::configure = [&](webfront::frontend::WKWebViewOptions& options) {
        options.downloadDirectory = folder;  // Created by the frontend when the download starts.
        options.downloadFinished  = [&](const std::string& path) {
            destination = path;
            done        = true;
            quitApplication();
        };
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?download");
    resetScenario();
    require(done, "The download did not finish");
    // The suggested name "../webfront-download.txt" must never leave the download folder. WebKit already neutralizes the
    // separators before the frontend sees the name (it arrives as "_webfront-download.txt"), so the folder is what is checked.
    const std::filesystem::path saved(destination);
    require(saved.parent_path() == folder && saved.filename().string().ends_with("webfront-download.txt"), ("Unexpected download destination: " + destination).c_str());
    require(readFile(destination) == "downloaded by WebFront", "The downloaded file has the wrong content");
    std::filesystem::remove_all(folder);
}

void chooseFile(bool cancel) {
    const auto fixture = std::filesystem::temp_directory_path() / ("webfront-wk-chooser-" + std::to_string(getpid()) + ".txt");
    std::ofstream(fixture) << "chosen by the test";
    bool asked                  = false;
    static std::string finalTitle;
    finalTitle.clear();
    ScenarioFrontend::configure = [&](webfront::frontend::WKWebViewOptions& options) {
        options.chooseFiles = [&]() -> std::vector<std::string> {
            asked = true;
            return cancel ? std::vector<std::string>{} : std::vector<std::string>{fixture.string()};
        };
    };
    ScenarioFrontend::started = [](Session& session) {
        // A chooser needs a user gesture, and evaluateJavaScript counts as one. The page reports through its title.
        Session* live = &session;
        auto     tick = ^(id timer) {
            const auto title = utf8(send(live->view, "title"));
            if (title.starts_with("picked:") || title == "cancelled") {
                finalTitle = title;
                send<void>(timer, "invalidate");
                quitApplication();
                return;
            }
            Object script(text("document.title === 'ready' && !window.clicked && (window.clicked = true, document.getElementById('picker').click())"));
            send<void>(live->view, "evaluateJavaScript:completionHandler:", script.get(), static_cast<id>(nullptr));
        };
        send(type("NSTimer"), "scheduledTimerWithTimeInterval:repeats:block:", 0.1, static_cast<BOOL>(true), tick);
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?choose");
    resetScenario();
    std::filesystem::remove(fixture);
    require(asked, "The file chooser was not requested");
    const std::string expected = cancel ? "cancelled" : "picked:chosen by the test";
    require(finalTitle == expected, ("Unexpected page state after the chooser: " + finalTitle).c_str());
}
void chooseOne() {
    chooseFile(false);
}
void chooseCancel() {
    chooseFile(true);
}

/// The save panel's "Replace" choice: the page's bytes land beside the old file and replace it only on success.
void replacedDownload() {
    Object     pool(send(type("NSAutoreleasePool"), "new"));
    const auto folder = std::filesystem::temp_directory_path() / ("webfront-wk-replace-" + std::to_string(getpid()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto target    = folder / "report.txt";
    const auto temporary = folder / "report.txt.part";
    std::vector<std::string> finished;
    Session session("http://127.0.0.1:0/", {.visible = false, .downloadFinished = [&](const std::string& path) {
                                                finished.push_back(path);
                                            }});
    const auto start = [&](std::uintptr_t key, std::string_view oldContent, std::string_view downloaded) {
        std::ofstream(target) << oldContent;
        if (!downloaded.empty())
            std::ofstream(temporary) << downloaded;
        session.downloadDestinations[reinterpret_cast<id>(key)] = {temporary.string(), target.string()};
    };
    start(1, "old", "new");
    session.downloadEnded(reinterpret_cast<id>(std::uintptr_t{1}), true);
    require(readFile(target) == "new" && !std::filesystem::exists(temporary), "A completed download did not replace the old file");
    require(finished == std::vector<std::string>{target.string()}, "The finished download was not reported at the chosen path");

    start(2, "old", "partial");
    session.downloadEnded(reinterpret_cast<id>(std::uintptr_t{2}), false);
    require(readFile(target) == "old" && !std::filesystem::exists(temporary), "A failed download damaged the old file or left its part behind");

    start(3, "old", "");  // The engine reported success but the part is gone: the replacement fails, the old file stays.
    session.downloadEnded(reinterpret_cast<id>(std::uintptr_t{3}), true);
    require(readFile(target) == "old" && finished.size() == 1, "A failed replacement was reported as a finished download");
    require(session.downloadDestinations.empty(), "A download outlived its end");
    std::filesystem::remove_all(folder);
}

void nativeEarlyClose() {
    nativeClose(false);
}
void quit() {
    nativeClose(true);
}
void navigationDiagnostic() {
    expectFailure(
        [] {
            Frontend::open("0", "index.html");
        },
        "initial navigation failed");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        checkWKWebViewHeader();
        const std::map<std::string_view, void (*)()> cases{
            {        "repeated-show",         repeatedShow},
            {          "main-thread",           mainThread},
            {         "native-close",     nativeEarlyClose},
            {                 "quit",                 quit},
            {      "startup-timeout",       startupTimeout},
            {      "redirect-policy",       redirectPolicy},
            {             "settings",             settings},
            {"navigation-diagnostic", navigationDiagnostic},
            {     "missing-response",      missingResponse},
            { "committed-navigation",  committedNavigation},
            {    "agent-application",     agentApplication},
            {             "download",             download},
            {          "choose-file",            chooseOne},
            {        "cancel-choose",         chooseCancel},
            {    "replaced-download",     replacedDownload}
        };
        cases.at(argc == 2 ? argv[1] : "")();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
