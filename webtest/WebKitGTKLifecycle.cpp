#include <system/NativeFS.hpp>
#include <WebFront.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>

#if defined(GTK_MAJOR_VERSION) || defined(G_TYPE_OBJECT) || defined(Bool) || defined(None) || defined(signals)
    #error WebFront.hpp must not expose GTK, GLib or X11 macros
#endif

namespace {
using Frontend = webfront::frontend::WebKitGTKFrontend;
using TestFS   = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
using App      = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, Frontend>;
namespace gtk  = webfront::frontend::detail::webkitgtk;

void expectFailure(auto&& operation, std::string_view diagnostic) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(diagnostic) != std::string_view::npos)
            return;
        throw;
    }
    throw std::runtime_error("Expected WebKitGTK failure was not reported");
}

void noDisplay() {
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    expectFailure(
        [] {
            Frontend::initialize();
        },
        "display");
}

void workerThread() {
    std::exception_ptr failure;
    std::jthread       worker([&] {
        try {
            expectFailure(
                [] {
                    Frontend::initialize();
                },
                "main thread");
            expectFailure(
                [] {
                    Frontend::open("9002", "index.html");
                },
                "main thread");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
}

void repeatedShow() {
    for (const auto page : {"no-bridge.html", "no-bridge.html?early-close"}) {
        App app("9002", std::filesystem::current_path());
        app.show(page);
    }
}

void httpError() {
    App app("9002", std::filesystem::current_path());
    expectFailure(
        [&] {
            app.show("missing-webkitgtk-page.html");
        },
        "HTTP error");
}

struct CloseDuringLoop {
    gtk::Session& session;
    unsigned      timer       = 0;
    bool          fired       = false;
    bool          loopRunning = false;

    explicit CloseDuringLoop(gtk::Session& live) : session(live) {
        timer = session.a.timeoutAdd(1, closeWindow, this);
    }
    ~CloseDuringLoop() {
        if (timer)
            session.a.sourceRemove(timer);
    }
    static int closeWindow(void* data) noexcept {
        auto& probe = *static_cast<CloseDuringLoop*>(data);
        if (!probe.session.a.loopIsRunning(probe.session.loop))
            return 1;
        probe.timer       = 0;
        probe.fired       = true;
        probe.loopRunning = true;
        probe.session.a.windowClose(probe.session.window);
        return 0;
    }
    void verify() const {
        if (!fired || !loopRunning)
            throw std::runtime_error("Native closure did not run inside the active GTK loop");
        if (!session.closed)
            throw std::runtime_error("Native closure did not close the session");
    }
};

struct NativeClosingFrontend : Frontend {
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        gtk::Session session(webfront::detail::BoundContentPolicy{webfront::frontend::detail::applicationUrl(address, port, ""), policy});
        session.create();
        CloseDuringLoop probe(session);
        session.run(webfront::frontend::detail::applicationUrl(address, port, page));
        probe.verify();
    }
};

void nativeClose() {
    using NativeApp = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, NativeClosingFrontend>;
    NativeApp app("9002", std::filesystem::current_path());
    app.show("no-bridge.html");
}

void preRunFailure() {
    // No GTK display or window is needed to preserve an error recorded by configure().
    gtk::Session session(webfront::detail::BoundContentPolicy{});
    session.failAndClose("failure before run");
    expectFailure(
        [&] {
            session.run("http://127.0.0.1:0/");
        },
        "failure before run");
}

void profilePermissions() {
    using enum std::filesystem::perms;
    for (const auto* variable : {"XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        const auto directory = gtk::profileDirectory(variable, ".unused");
        if (std::filesystem::status(directory).permissions() != owner_all)
            throw std::runtime_error("New profile directory is not private");
        std::filesystem::permissions(directory, all, std::filesystem::perm_options::replace);
        gtk::profileDirectory(variable, ".unused");
        if (std::filesystem::status(directory).permissions() != owner_all)
            throw std::runtime_error("Existing profile directory was not restricted to 0700");
    }
}

void run(std::string_view mode) {
    const std::map<std::string_view, void (*)()> cases{
        {         "no-display",          noDisplay},
        {      "worker-thread",       workerThread},
        {      "repeated-show",       repeatedShow},
        {         "http-error",          httpError},
        {       "native-close",        nativeClose},
        {    "pre-run-failure",      preRunFailure},
        {"profile-permissions", profilePermissions}
    };
    const auto selected = cases.find(mode);
    if (selected == cases.end())
        throw std::runtime_error("Unknown WebKitGTK scenario");
    selected->second();
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            return 1;
        run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
