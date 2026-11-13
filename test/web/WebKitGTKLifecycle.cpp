#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <array>
#include <chrono>
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

/// Runs a page of the test folder in a session configured by the scenario, for at most 45 seconds.
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
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            unsigned timer = 0;
            std::exception_ptr failure{};
        } poll{session};
        poll.timer = session.a.timeoutAdd(
            100,
            [](void* data) -> int {
                auto& probe = *static_cast<Poll*>(data);
                if (probe.session.closed) {
                    probe.timer = 0;  // Returning 0 removes the source: it must not be removed again.
                    return 0;
                }
                bool done = false;
                try {
                    done = finished(probe.session);
                } catch (...) {
                    probe.failure = std::current_exception();
                    done = true;
                }
                if (done || std::chrono::steady_clock::now() >= probe.deadline) {
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
        if (poll.failure)
            std::rethrow_exception(poll.failure);
    }
};

using ScenarioApp = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, ScenarioFrontend>;

void pollFailure() {
    ScenarioFrontend::configure = [](gtk::Session&) {};
    ScenarioFrontend::finished = [](gtk::Session&) -> bool {
        throw std::runtime_error("poll failure");
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    expectFailure([&] { app.show("no-bridge.html"); }, "poll failure");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open downloaded file: " + path.string());
    std::string result;
    std::array<char, 4096> buffer;
    while (stream.read(buffer.data(), buffer.size()) || stream.gcount() != 0)
        result.append(buffer.data(), static_cast<std::size_t>(stream.gcount()));
    if (stream.bad())
        throw std::runtime_error("Cannot read downloaded file: " + path.string());
    return result;
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
    void* x11           = dlopen("libX11.so.6", RTLD_NOW);
    void* xtest         = dlopen("libXtst.so.6", RTLD_NOW);
    void* gtkModule     = dlopen("libgtk-4.so.1", RTLD_NOW | RTLD_LOCAL);
    using Open          = void* (*)(const char*);
    using Flush         = int (*)(void*);
    using Move          = int (*)(void*, int, int, int, unsigned long);
    using Press         = int (*)(void*, unsigned, int, unsigned long);
    using Translate     = int (*)(void*, unsigned long, unsigned long, int, int, int*, int*, unsigned long*);
    using Root          = unsigned long (*)(void*);
    using Surface       = void* (*)(void*);
    using Xid           = unsigned long (*)(void*);
    using Mapped        = int (*)(void*);
    Open      open      = x11 ? reinterpret_cast<Open>(dlsym(x11, "XOpenDisplay")) : nullptr;
    Flush     flush     = x11 ? reinterpret_cast<Flush>(dlsym(x11, "XFlush")) : nullptr;
    Move      move      = xtest ? reinterpret_cast<Move>(dlsym(xtest, "XTestFakeMotionEvent")) : nullptr;
    Press     press     = xtest ? reinterpret_cast<Press>(dlsym(xtest, "XTestFakeButtonEvent")) : nullptr;
    Translate translate = x11 ? reinterpret_cast<Translate>(dlsym(x11, "XTranslateCoordinates")) : nullptr;
    Root      root      = x11 ? reinterpret_cast<Root>(dlsym(x11, "XDefaultRootWindow")) : nullptr;
    // GTK's client surface need not start at the root window's origin, even under Xvfb.
    Surface surface = gtkModule ? reinterpret_cast<Surface>(dlsym(gtkModule, "gtk_native_get_surface")) : nullptr;
    Xid     xid     = gtkModule ? reinterpret_cast<Xid>(dlsym(gtkModule, "gdk_x11_surface_get_xid")) : nullptr;
    Mapped  mapped  = gtkModule ? reinterpret_cast<Mapped>(dlsym(gtkModule, "gtk_widget_get_mapped")) : nullptr;
    void*   display = open ? open(nullptr) : nullptr;

    bool available() const {
        return display && move && press && flush && translate && root && surface && xid && mapped;
    }
    void click(void* window, int x, int y, int phase) const {
        const auto native = surface(window);
        if (!native)
            return;
        int           rootX = 0, rootY = 0;
        unsigned long child = 0;
        if (!translate(display, xid(native), root(display), x, y, &rootX, &rootY, &child))
            throw std::runtime_error("Cannot locate the GTK file chooser in the X root window");
        // One step per poll: GTK and the web process must see the pointer enter before they receive the button events.
        switch (phase) {
            case 0:
                move(display, 0, rootX + 120, rootY + 60, 0);
                break;
            case 1:
                move(display, 0, rootX, rootY, 0);
                break;
            case 2:
                press(display, 1, 1, 0);
                break;
            case 3:
                press(display, 1, 0, 0);
                break;
            default:
                break;
        }
        flush(display);
    }
};

void chooseFile(bool cancel) {
    const auto fixture = std::filesystem::temp_directory_path() / ("webfront-chooser-" + std::to_string(getpid()) + ".txt");
    std::ofstream(fixture) << "chosen by the test";
    bool asked                  = false;
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
        if (title == "ready" && pointer.mapped(session.window) && !asked)
            pointer.click(session.window, 40, 20, ticks++ % 5);  // The button's position within the GTK client surface.
        return title.starts_with("picked:") || title == "cancelled";
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?choose");
    std::filesystem::remove(fixture);
    if (!asked)
        throw std::runtime_error("The file chooser was not requested (page title \"" + title + "\", " +
                                 std::to_string(ticks / 5 + (ticks % 5 == 4)) + " clicks, " + std::to_string(ticks) + " pointer steps)");
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
        if (title == "ready" && pointer.mapped(session.window) && !opened)
            pointer.click(session.window, 40, 20, ticks++ % 5);
        return opened && ++open > 10;  // Let the dialog map, then close the window under it.
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("file-services.html?choose");
    if (!opened)
        throw std::runtime_error("The file dialog was not opened (page title \"" + title + "\", " +
                                 std::to_string(ticks / 5 + (ticks % 5 == 4)) + " clicks, " + std::to_string(ticks) + " pointer steps)");
}

/// alert, confirm and prompt reach the injection point and their answers reach the page (issue #452).
void scriptDialogs() {
    std::vector<std::string> seen;
    ScenarioFrontend::configure = [&](gtk::Session& session) {
        session.answerScriptDialog = [&](const webfront::frontend::ScriptDialog& dialog) {
            using Kind = webfront::frontend::ScriptDialog::Kind;
            seen.push_back(std::to_string(static_cast<int>(dialog.kind)) + ":" + dialog.message + ":" + dialog.defaultText);
            if (dialog.kind == Kind::prompt)
                return dialog.message == "name?" ? webfront::frontend::ScriptDialogAnswer{true, "typed"} : webfront::frontend::ScriptDialogAnswer{};
            return webfront::frontend::ScriptDialogAnswer{true, {}};
        };
    };
    std::string title;
    ScenarioFrontend::finished = [&](gtk::Session& session) {
        const char* current = session.a.viewTitle(session.view);
        title               = current ? current : "";
        return title.starts_with("answers:");
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("script-dialogs.html");
    if (title != "answers:true|typed|null")
        throw std::runtime_error("Unexpected answers from the script dialogs: " + title);
    const std::vector<std::string> expected{"0:hello:", "1:sure?:", "2:name?:default", "2:cancelled?:x"};
    if (seen != expected)
        throw std::runtime_error("The injection did not see the expected dialogs");
}

/// Closing the window while WebKitGTK's own script dialog is open must end cleanly, without waiting for an answer.
void closeWithScriptDialog() {
    using Clock = std::chrono::steady_clock;
    bool              opened = false;
    Clock::time_point openedAt;
    ScenarioFrontend::configure = [&](gtk::Session& session) {
        session.scriptDialogOpened = [&] {
            opened   = true;
            openedAt = Clock::now();
        };
    };
    ScenarioFrontend::finished = [&](gtk::Session&) {
        return opened && Clock::now() - openedAt > std::chrono::milliseconds(500);  // Let the dialog map, then close under it.
    };
    ScenarioApp app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    app.show("script-dialogs.html?hold");
    if (!opened)
        throw std::runtime_error("The script dialog was not opened");
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
        {       "poll-failure",         pollFailure},
        {"profile-permissions", profilePermissions},
        {       "window-title",        windowTitle},
        {          "download",           download},
        {"download-user-dir", downloadUserDirectory},
        { "close-with-dialog",      closeWithDialog},
        {       "choose-file",          chooseOne},
        {     "cancel-choose",       chooseCancel},
        {    "script-dialogs",       scriptDialogs},
        {"close-with-script-dialog", closeWithScriptDialog}
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
