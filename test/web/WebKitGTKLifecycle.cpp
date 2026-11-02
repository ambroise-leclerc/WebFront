#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <functional>
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
        App app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
        app.show(page);
    }
}

void httpError() {
    App app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
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
    NativeApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("no-bridge.html");
}

/// Runs a page of the test folder in a session configured by the scenario, for at most `seconds`.
struct ScenarioFrontend : Frontend {
    static inline std::function<void(gtk::Session&)> configure;
    static inline std::function<bool(gtk::Session&)> finished;  // Polled; true closes the window.
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        gtk::Session session(webfront::detail::BoundContentPolicy{webfront::frontend::detail::applicationUrl(address, port, ""), policy});
        session.create();
        configure(session);
        struct Poll {
            gtk::Session& session;
            int           remaining = 150;  // 15 s
            unsigned      timer     = 0;
        } poll{session, 150, 0};
        poll.timer = session.a.timeoutAdd(
            100,
            [](void* data) -> int {
                auto& probe = *static_cast<Poll*>(data);
                if (probe.session.closed) {
                    probe.timer = 0;  // Returning 0 removes the source: it must not be removed again.
                    return 0;
                }
                if (finished(probe.session) || --probe.remaining == 0) {
                    probe.timer = 0;
                    probe.session.close();
                    return 0;
                }
                return 1;
            },
            &poll);
        session.run(webfront::frontend::detail::applicationUrl(address, port, page));
        if (poll.timer)
            session.a.sourceRemove(poll.timer);
    }
};

using ScenarioApp = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, ScenarioFrontend>;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void download() {
    const auto parent = std::filesystem::temp_directory_path() / ("webfront-download-" + std::to_string(getpid()));
    const auto folder = parent / "not" / "yet";  // The frontend creates the folder: WebKitGTK writes into an existing one only.
    std::filesystem::remove_all(parent);
    std::string destination;
    bool        done = false;
    ScenarioFrontend::configure = [&](gtk::Session& session) {
        session.downloadDirectory = folder;
        session.downloadFinished  = [&](const std::string& path) {
            destination = path;
            done        = true;
        };
    };
    ScenarioFrontend::finished = [&](gtk::Session&) {
        return done;
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?download");
    if (!done)
        throw std::runtime_error("The download did not finish");
    // The suggested name "../webfront-download.txt" must never leave the download folder.
    if (std::filesystem::path(destination) != folder / "webfront-download.txt")
        throw std::runtime_error("Unexpected download destination: " + destination);
    if (readFile(destination) != "downloaded by WebFront")
        throw std::runtime_error("The downloaded file has the wrong content");
    std::filesystem::remove_all(parent);
}

/// Without a configured folder, the download lands in GLib's user download directory (user-dirs.dirs), not in an
/// XDG_DOWNLOAD_DIR environment variable, which nothing defines in a normal session.
void downloadUserDirectory() {
    const auto base = std::filesystem::temp_directory_path() / ("webfront-userdir-" + std::to_string(getpid()));
    std::filesystem::remove_all(base);
    const auto config = base / "config";
    const auto folder = base / "Telechargements";
    const auto decoy  = base / "decoy";
    std::filesystem::create_directories(config);
    std::filesystem::create_directories(folder);
    std::filesystem::create_directories(decoy);
    std::ofstream(config / "user-dirs.dirs") << "XDG_DOWNLOAD_DIR=\"" << folder.string() << "\"\n";
    // GLib reads the configuration once, at the first query: set it before any session exists.
    setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    setenv("XDG_DOWNLOAD_DIR", decoy.c_str(), 1);
    std::string destination;
    bool        done        = false;
    ScenarioFrontend::configure = [&](gtk::Session& session) {
        session.downloadFinished = [&](const std::string& path) {
            destination = path;
            done        = true;
        };
    };
    ScenarioFrontend::finished = [&](gtk::Session&) {
        return done;
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?download");
    if (!done)
        throw std::runtime_error("The download did not finish");
    if (std::filesystem::path(destination) != folder / "webfront-download.txt")
        throw std::runtime_error("The download ignored GLib's user download directory: " + destination);
    std::filesystem::remove_all(base);
}

/// A real pointer click through the X server's XTest extension: web content only opens a file chooser for a user gesture.
struct FakeClick {
    void* x11   = dlopen("libX11.so.6", RTLD_NOW);
    void* xtest = dlopen("libXtst.so.6", RTLD_NOW);
    using Open  = void* (*)(const char*);
    using Flush = int (*)(void*);
    using Move  = int (*)(void*, int, int, int, unsigned long);
    using Press = int (*)(void*, unsigned, int, unsigned long);
    Open  open    = x11 ? reinterpret_cast<Open>(dlsym(x11, "XOpenDisplay")) : nullptr;
    Flush flush   = x11 ? reinterpret_cast<Flush>(dlsym(x11, "XFlush")) : nullptr;
    Move  move    = xtest ? reinterpret_cast<Move>(dlsym(xtest, "XTestFakeMotionEvent")) : nullptr;
    Press press   = xtest ? reinterpret_cast<Press>(dlsym(xtest, "XTestFakeButtonEvent")) : nullptr;
    void* display = open ? open(nullptr) : nullptr;

    bool available() const {
        return display && move && press && flush;
    }
    void click(int x, int y) const {
        // Without a window manager the pointer may already rest at the target: a press with no motion before it can be
        // delivered before the page knows the pointer is over it. Arrive from a neighbouring point.
        move(display, 0, x + 6, y + 6, 0);
        flush(display);
        move(display, 0, x, y, 0);
        press(display, 1, 1, 0);
        press(display, 1, 0, 0);
        flush(display);
    }
};

void chooseFile(bool cancel) {
    const auto fixture = std::filesystem::temp_directory_path() / ("webfront-chooser-" + std::to_string(getpid()) + ".txt");
    std::ofstream(fixture) << "chosen by the test";
    bool asked = false;
    ScenarioFrontend::configure = [&](gtk::Session& session) {
        session.chooseFiles = [&]() -> std::vector<std::string> {
            asked = true;
            return cancel ? std::vector<std::string>{} : std::vector<std::string>{fixture.string()};
        };
    };
    std::string     title;
    const FakeClick pointer;
    if (!pointer.available())
        throw std::runtime_error("XTest is unavailable: run the scenario under Xvfb with libXtst");
    int ticks = 0;
    ScenarioFrontend::finished = [&](gtk::Session& session) {
        const char* current = session.a.viewTitle(session.view);
        title               = current ? current : "";
        if (title == "ready" && !asked && ++ticks % 5 == 1)
            pointer.click(40, 20);  // The "Choose File" button, at the page's top-left corner.
        return title.starts_with("picked:") || title == "cancelled";
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?choose");
    std::filesystem::remove(fixture);
    if (!asked)
        throw std::runtime_error("The file chooser was not requested (page title \"" + title + "\", " + std::to_string(ticks) + " clicks)");
    const std::string expected = cancel ? "cancelled" : "picked:chosen by the test";
    if (title != expected)
        throw std::runtime_error("Unexpected page state after the chooser: " + title);
}
void chooseOne() {
    chooseFile(false);
}
void chooseCancel() {
    chooseFile(true);
}

/// Closing the window while the system file dialog is open must end cleanly, without waiting for an answer.
void closeWithDialog() {
    bool opened = false;
    int  open   = 0;  // Poll ticks since the dialog opened.
    ScenarioFrontend::configure = [&](gtk::Session& session) {
        session.chooserOpened = [&] {
            opened = true;
        };
    };
    std::string     title;
    const FakeClick pointer;
    if (!pointer.available())
        throw std::runtime_error("XTest is unavailable: run the scenario under Xvfb with libXtst");
    int ticks = 0;
    ScenarioFrontend::finished = [&](gtk::Session& session) {
        const char* current = session.a.viewTitle(session.view);
        title               = current ? current : "";
        if (title == "ready" && !opened && ++ticks % 5 == 1)
            pointer.click(40, 20);
        return opened && ++open > 10;  // Let the dialog map, then close the window under it.
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?choose");
    if (!opened)
        throw std::runtime_error("The file dialog was not opened");
}

void windowTitle() {
    gtk::initialize();
    const auto titleOf = [](webfront::WindowOptions window) {
        gtk::Session session(webfront::detail::BoundContentPolicy{}, std::move(window));
        session.create();
        const char* native = session.a.windowGetTitle(session.window);
        return std::string(native ? native : "");  // The session destructor tears the window down; no navigation is needed.
    };
    if (titleOf({.title = "SiliconForge"}) != "SiliconForge")
        throw std::runtime_error("The application's window title was not applied");
    const auto defaultTitle = titleOf({});
    if (defaultTitle.empty() || defaultTitle == "WebFront Application")
        throw std::runtime_error("The default window title must be the executable's name");
    if (defaultTitle != "webkitgtk_lifecycle")
        throw std::runtime_error("Unexpected default window title: " + defaultTitle);
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
        {"profile-permissions", profilePermissions},
        {       "window-title",        windowTitle},
        {          "download",           download},
        {"download-user-dir", downloadUserDirectory},
        { "close-with-dialog",      closeWithDialog},
        {       "choose-file",          chooseOne},
        {     "cancel-choose",       chooseCancel}
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
