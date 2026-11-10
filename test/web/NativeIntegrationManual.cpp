// Manual validation of the native integration of the frontends (#450: the interface-thread executor of #462 and the
// native window of #463) with a real browser or window and a real native dialog. Never built or run by CTest/CI:
//
//   cmake --build <build> --target native_integration_manual
//   <build>/test/web/native_integration_manual browser  [dialog-ms]   system browser with the interface-thread loop
//   <build>/test/web/native_integration_manual embedded [dialog-ms]   window of the test frontend (WEBFRONT_WEBTEST_FRONTEND)
//
// It opens a page, and a native file dialog that closes by itself after `dialog-ms` (1500 by default; raise it to look at
// the dialog). `[ok]`/`[FAIL]` lines are checked by the program and decide the exit status; `[look]` lines are what only
// a person can confirm. Results are recorded in docs/validation/native-integration-450.md.
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
    #include <shobjidl.h>
    #include <windows.h>
#elif defined(__linux__)
    #include <dlfcn.h>
#endif

namespace {

namespace ex = webfront::networking::ex;
using webfront::frontend::NativeWindowError;
using webfront::frontend::NativeWindowKind;
using webfront::frontend::NativeWindowResult;
using webfront::frontend::UIExecutor;

std::atomic<int> failures{0};
std::mutex       printMutex;

void line(std::string_view tag, const std::string& what) {
    std::scoped_lock lock{printMutex};
    std::cout << "  " << tag << ' ' << what << std::endl;
}
void check(bool ok, const std::string& what) {
    line(ok ? "[ok]  " : "[FAIL]", what);
    if (!ok)
        ++failures;
}
void info(const std::string& what) {
    line("[info]", what);
}
void look(const std::string& what) {
    line("[look]", what);
}

std::string text(const NativeWindowResult& result) {
    if (result) {
        std::string kind;
        switch (result->kind) {
            case NativeWindowKind::win32:
                kind = "win32";
                break;
            case NativeWindowKind::cocoa:
                kind = "cocoa";
                break;
            case NativeWindowKind::gtk:
                kind = "gtk";
                break;
            case NativeWindowKind::cef:
                kind = "cef";
                break;
        }
        return "window kind=" + kind + " portalParent='" + result->portalParent + "'";
    }
    switch (result.error()) {
        case NativeWindowError::unsupported:
            return "error unsupported";
        case NativeWindowError::notEmbedded:
            return "error notEmbedded";
        case NativeWindowError::notOpen:
            return "error notOpen";
        case NativeWindowError::closed:
            return "error closed";
        case NativeWindowError::wrongThread:
            return "error wrongThread";
    }
    return "error ?";
}

enum class Outcome { ran, stopped, unavailable, failed };

template <typename F>
struct Receiver {
    using receiver_concept = ex::receiver_tag;
    std::promise<Outcome>* result;
    F                      function;
    auto                   get_env() const noexcept {
        return ex::prop{ex::get_stop_token, ex::never_stop_token{}};
    }
    void set_value() && noexcept {
        // noexcept: an exception of the operation is an outcome of the scenario, not a std::terminate. The outcome is
        // published once, outside the try block, so that a failing set_value() is not retried.
        auto outcome = Outcome::ran;
        try {
            function();
        } catch (...) {
            outcome = Outcome::failed;
        }
        try {
            result->set_value(outcome);
        } catch (...) {
            // The promise was already satisfied or its state is gone: nobody waits for this outcome any more.
        }
    }
    void set_stopped() && noexcept {
        result->set_value(Outcome::stopped);
    }
    void set_error(std::exception_ptr error) && noexcept {
        try {
            std::rethrow_exception(error);
        } catch (const webfront::frontend::UIExecutorUnavailable&) {
            result->set_value(Outcome::unavailable);
        } catch (...) {
            result->set_value(Outcome::failed);
        }
    }
};

// Schedules `function` on the interface thread. The operation state lives until the program ends: a completion that
// arrives after its wait timed out still finds it.
template <typename F>
std::future<Outcome> onUI(const UIExecutor& executor, F function) {
    struct State {
        std::promise<Outcome>                                                                             result;
        decltype(ex::connect(std::declval<decltype(executor.schedule())>(), std::declval<Receiver<F>>())) operation;
        State(const UIExecutor& e, F f) : operation(ex::connect(e.schedule(), Receiver<F>{&result, std::move(f)})) { }
    };
    static std::list<std::unique_ptr<State>> states;
    static std::mutex                        mutex;
    State*                                   state = nullptr;
    {
        std::scoped_lock lock{mutex};
        state = states.emplace_back(std::make_unique<State>(executor, std::move(function))).get();
    }
    auto future = state->result.get_future();
    ex::start(state->operation);
    return future;
}

bool ready(std::future<Outcome>& future, Outcome expected, int seconds = 20) {
    return future.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready && future.get() == expected;
}

#if defined(_WIN32)

// The visible dialog window of this process: the file dialog is a #32770 top-level window.
HWND findDialog() {
    HWND found{};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            wchar_t name[64]{};
            GetClassNameW(window, name, 64);
            if (process == GetCurrentProcessId() && IsWindowVisible(window) && std::wstring_view(name) == L"#32770") {
                *reinterpret_cast<HWND*>(parameter) = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// On the interface thread: a real IFileOpenDialog (COM single-threaded apartment, modal loop), probed and then closed
// by a helper thread. `parent` is null for the system browser.
void nativeDialog(const UIExecutor& executor, HWND parent, int visibleMs) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    check(SUCCEEDED(initialized), "COM single-threaded apartment on the interface thread");
    IFileOpenDialog* dialog  = nullptr;
    const auto       created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    check(SUCCEEDED(created) && dialog, "IFileOpenDialog created");
    if (dialog) {
        dialog->SetTitle(L"WebFront native integration (closes by itself)");
        HWND shownDialog{};
        HWND owner{};
        bool parentDisabled{};
        bool ranDuringDialog{};
        {
            std::jthread helper([&] {
                for (int attempt = 0; attempt < 200 && !shownDialog; ++attempt) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    shownDialog = findDialog();
                }
                if (!shownDialog)
                    return;
                owner          = GetWindow(shownDialog, GW_OWNER);
                parentDisabled = parent && !IsWindowEnabled(parent);
                // The dialog's modal loop must keep dispatching the executor's wake-up message.
                auto during     = onUI(executor, [] { });
                ranDuringDialog = ready(during, Outcome::ran, 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(visibleMs));
                PostMessageW(shownDialog, WM_CLOSE, 0, 0);
            });
            const auto   shown = dialog->Show(parent);
            helper.join();
            check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED), "Show() returned once the dialog was closed (cancelled)");
        }
        dialog->Release();
        check(shownDialog != nullptr, "the native dialog appeared");
        check(ranDuringDialog, "an operation scheduled while the dialog is modal ran on the interface thread");
        if (parent) {
            check(owner == parent, "the dialog is owned by the application's window");
            check(parentDisabled, "the application's window is blocked while the dialog is open");
            check(IsWindowEnabled(parent) != FALSE, "the application's window is enabled again after the dialog");
            check(GetForegroundWindow() == parent || GetActiveWindow() == parent, "the application's window is active again after the dialog");
        } else {
            check(owner == nullptr, "the dialog has no owner (system browser)");
        }
    }
    if (SUCCEEDED(initialized))
        CoUninitialize();
}

#elif defined(__linux__)

template <typename T>
T symbol(const char* library, const char* name) {
    void* module = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    return module ? reinterpret_cast<T>(dlsym(module, name)) : nullptr;
}

// On the interface thread: the loop is GLib's, and GIO (GDBus, the portal's transport chosen by ADR-020) loads at run time.
void glibChecks() {
    check(dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_GLOBAL) != nullptr, "libgio-2.0.so.0 (GDBus, the portal's transport) loads at run time");
    const auto isOwner = symbol<int (*)(void*)>("libglib-2.0.so.0", "g_main_context_is_owner");
    const auto context = symbol<void* (*)()>("libglib-2.0.so.0", "g_main_context_default");
    check(isOwner && context && isOwner(context()), "the operation runs inside the GLib default main context, owned by this thread");
}

// A GtkFileDialog (GTK 4.10 or later; it goes through the desktop portal when there is one) parented to the window,
// cancelled after `visibleMs` by an operation of the executor. Called from the scenario thread.
void gtkDialog(const UIExecutor& executor, void* parent, int visibleMs) {
    struct Dialog {
        void*                     object{};
        void*                     cancellable{};
        std::promise<std::string> finished;
        std::thread::id           callbackThread;
    };
    static Dialog dialog;  // the GTK callback may outlive a wait that timed out
    using Callback                = void (*)(void*, void*, void*);
    const auto        create      = symbol<void* (*)()>("libgtk-4.so.1", "gtk_file_dialog_new");
    const auto        title       = symbol<void (*)(void*, const char*)>("libgtk-4.so.1", "gtk_file_dialog_set_title");
    const auto        open        = symbol<void (*)(void*, void*, void*, Callback, void*)>("libgtk-4.so.1", "gtk_file_dialog_open");
    static const auto finish      = symbol<void* (*)(void*, void*, void**)>("libgtk-4.so.1", "gtk_file_dialog_open_finish");
    const auto        isActive    = symbol<int (*)(void*)>("libgtk-4.so.1", "gtk_window_is_active");
    const auto        cancellable = symbol<void* (*)()>("libgio-2.0.so.0", "g_cancellable_new");
    const auto        cancel      = symbol<void (*)(void*)>("libgio-2.0.so.0", "g_cancellable_cancel");
    static const auto unref       = symbol<void (*)(void*)>("libgobject-2.0.so.0", "g_object_unref");
    static const auto freeError   = symbol<void (*)(void*)>("libglib-2.0.so.0", "g_error_free");
    if (!create || !title || !open || !finish || !isActive || !cancellable || !cancel || !unref || !freeError) {
        check(false, "GtkFileDialog is available (GTK 4.10 or later)");
        return;
    }
    auto opened = onUI(executor, [&] {
        dialog.object      = create();
        dialog.cancellable = cancellable();
        title(dialog.object, "WebFront native integration (closes by itself)");
        open(
            dialog.object,
            parent,
            dialog.cancellable,
            [](void* source, void* result, void*) {
                struct Error {
                    unsigned domain;
                    int      code;
                    char*    message;
                };
                void*       error   = nullptr;
                void*       file    = finish(source, result, &error);
                std::string outcome = file ? "a file was chosen" : "no file";
                if (error) {
                    outcome += std::string(" (") + static_cast<Error*>(error)->message + ")";
                    freeError(error);
                }
                if (file)
                    unref(file);
                dialog.callbackThread = std::this_thread::get_id();
                dialog.finished.set_value(outcome);
            },
            nullptr);
    });
    check(ready(opened, Outcome::ran), "the parented GtkFileDialog was opened from an operation of the executor");
    look("the dialog opened over the application's window, stays on top of it and blocks it");
    std::this_thread::sleep_for(std::chrono::milliseconds(visibleMs));
    std::thread::id interfaceThread;
    auto            cancelled = onUI(executor, [&] {
        interfaceThread = std::this_thread::get_id();
        cancel(dialog.cancellable);
    });
    check(ready(cancelled, Outcome::ran), "an operation scheduled while the dialog is open ran on the interface thread");
    auto finished = dialog.finished.get_future();
    if (finished.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        check(false, "the dialog completed after its cancellation");
        return;
    }
    info("dialog result: " + finished.get());
    check(dialog.callbackThread == interfaceThread, "the dialog completed on the interface thread");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    bool active    = false;
    auto activated = onUI(executor, [&] {
        active = isActive(parent) != 0;
        unref(dialog.object);
        unref(dialog.cancellable);
    });
    check(ready(activated, Outcome::ran), "the window was read after the dialog");
    info(std::string("gtk_window_is_active after the dialog: ") + (active ? "true" : "false"));
    look("focus returned to the application's window");
}

// The portal identifier of #463: `x11:<XID>` or `wayland:<handle>`, tried every 100 ms, 70 times (about 7 s), after the window is presented.
template <typename App>
void portalParent(App& app, const UIExecutor& executor) {
    std::string identifier;
    for (int attempt = 0; attempt < 70 && identifier.empty(); ++attempt) {
        auto read = onUI(executor, [&] {
            if (const auto window = app.nativeWindow().get())
                identifier = window->portalParent;
        });
        if (!ready(read, Outcome::ran))
            break;
        if (identifier.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto* session = std::getenv("XDG_SESSION_TYPE");
    const auto* wayland = std::getenv("WAYLAND_DISPLAY");
    const auto* backend = std::getenv("GDK_BACKEND");
    info(std::string("XDG_SESSION_TYPE=") + (session ? session : "") + " WAYLAND_DISPLAY=" + (wayland ? wayland : "")
         + " GDK_BACKEND=" + (backend ? backend : ""));
    info("portalParent: '" + identifier + "'");
    check(identifier.starts_with("x11:") || identifier.starts_with("wayland:"),
          "the window exports a portal parent identifier (x11:<XID> or wayland:<handle>)");
}

#endif

template <typename Frontend>
int validate(int visibleMs) {
    using App           = webfront::WebFrontWithFrontend<Frontend>;
    const bool embedded = !std::same_as<Frontend, webfront::frontend::BrowserWithUILoopFrontend>;
    std::cout << (embedded ? "Embedded window" : "System browser with the interface-thread loop") << std::endl;
    App        app(webfront::WebFrontConfig{.port = "0"});
    const auto executor   = app.uiExecutor();
    const auto mainThread = std::this_thread::get_id();
    check(!executor.available(), "the executor is unavailable before show()");
    check(text(app.nativeWindow().get()) == (embedded ? "error notOpen" : "error unsupported"),
          "app.nativeWindow() before show(): " + text(app.nativeWindow().get()));
    {
        auto early = onUI(executor, [] { });
        check(ready(early, Outcome::unavailable), "an operation scheduled before show() is refused (UIExecutorUnavailable)");
    }

    std::promise<typename App::UI> connectedPromise;
    std::atomic<bool>              connectedOnce{false};
    app.onUIStarted([&](typename App::UI ui) {
        if (!connectedOnce.exchange(true))
            connectedPromise.set_value(ui);
    });

    std::atomic<bool> scenarioDone{false};
    std::jthread      scenario([&] {
        auto connected = connectedPromise.get_future();
        if (connected.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
            check(false, "a browser connected within 60 s");
        } else {
            auto ui = connected.get();
            check(true, "a browser connected through the bridge");
            check(executor.available(), "the executor is available while the loop runs");
            check(!executor.onUIThread(), "onUIThread() is false on a worker thread");
            check(ui.isEmbeddedView() == embedded, std::string("ui.isEmbeddedView() is ") + (embedded ? "true" : "false"));
            check(text(app.nativeWindow().get()) == (embedded ? "error wrongThread" : "error unsupported"),
                  "app.nativeWindow().get() on a worker thread: " + text(app.nativeWindow().get()));

            void*            pointer = nullptr;
            NativeWindowKind kind{};
            auto             first = onUI(executor, [&] {
                check(std::this_thread::get_id() == mainThread, "the operation runs on the thread that called show()");
                check(executor.onUIThread(), "onUIThread() is true inside the operation");
                const auto fromApp = app.nativeWindow().get();
                const auto fromUI  = ui.nativeWindow();
                info("app.nativeWindow().get(): " + text(fromApp));
                info("ui.nativeWindow():        " + text(fromUI));
                if (embedded) {
                    check(fromApp.has_value() && fromUI.has_value() && fromApp->pointer == fromUI->pointer, "the connection's window is the frontend's window");
                    if (fromUI) {
                        pointer = fromUI->pointer;
                        kind    = fromUI->kind;
                    }
                } else {
                    check(!fromApp && fromApp.error() == NativeWindowError::unsupported, "the system browser frontend has no native window");
                    check(!fromUI && fromUI.error() == NativeWindowError::notEmbedded, "the system browser connection has no parent window (notEmbedded)");
                }
            });
            check(ready(first, Outcome::ran), "the first operation completed with set_value");

            int  order = 0, a = 0, b = 0, c = 0;
            auto fa = onUI(executor, [&] {
                a = ++order;
            });
            auto fb = onUI(executor, [&] {
                b = ++order;
            });
            auto fc = onUI(executor, [&] {
                c = ++order;
            });
            check(ready(fa, Outcome::ran) && ready(fb, Outcome::ran) && ready(fc, Outcome::ran) && a == 1 && b == 2 && c == 3,
                  "three operations ran in scheduling order");
#if defined(_WIN32)
            const auto window = kind == NativeWindowKind::win32 ? static_cast<HWND>(pointer) : nullptr;
            auto       dialog = onUI(executor, [&] {
                if (window) {
                    DWORD      process = 0;
                    const auto thread  = GetWindowThreadProcessId(window, &process);
                    check(IsWindow(window) && GetAncestor(window, GA_ROOT) == window, "the HWND is a live top-level window");
                    check(process == GetCurrentProcessId() && thread == GetCurrentThreadId(), "the HWND belongs to the interface thread of this process");
                }
                nativeDialog(executor, window, visibleMs);
            });
            check(ready(dialog, Outcome::ran, 60 + visibleMs / 1000), "the native dialog operation completed");
            if (embedded && !window)
                info("no HWND for this frontend: the dialog was not parented");
#elif defined(__linux__)
            auto glib = onUI(executor, glibChecks);
            check(ready(glib, Outcome::ran), "the GLib operation completed");
            if (kind == NativeWindowKind::gtk && pointer) {
                portalParent(app, executor);
                gtkDialog(executor, pointer, visibleMs);
            } else if (embedded) {
                info("no GtkWindow for this frontend: no parented dialog was opened");
            }
#else
            info("no native dialog is opened by this program on this platform");
#endif
            // Ends the loop: the browser's by its stop request, the embedded window's by closing the window.
            if constexpr (requires { Frontend::requestStop(); }) {
                Frontend::requestStop();
            } else {
                bool closing = false;
#if defined(_WIN32)
                if (window) {
                    auto close = onUI(executor, [&] {
                        PostMessageW(window, WM_CLOSE, 0, 0);
                    });
                    closing    = ready(close, Outcome::ran);
                }
#elif defined(__linux__)
                const auto closeWindow = symbol<void (*)(void*)>("libgtk-4.so.1", "gtk_window_close");
                if (kind == NativeWindowKind::gtk && pointer && closeWindow) {
                    auto close = onUI(executor, [&] {
                        closeWindow(pointer);
                    });
                    closing    = ready(close, Outcome::ran);
                }
#endif
                if (!closing)
                    look("close the application's window to finish");
            }
        }
        scenarioDone = true;
        if constexpr (requires { Frontend::requestStop(); })
            Frontend::requestStop();
    });

    app.show();
    scenario.join();
    check(scenarioDone, "show() returned after the scenario ended the loop");
    check(!executor.available() && !executor.onUIThread(), "the executor is unavailable after the loop ended");
    {
        auto late = onUI(executor, [] { });
        check(ready(late, Outcome::unavailable), "an operation scheduled after closure is refused");
    }
    check(text(app.nativeWindow().get()) == (embedded ? "error closed" : "error unsupported"),
          "app.nativeWindow() after the loop: " + text(app.nativeWindow().get()));
    std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL (" + std::to_string(failures.load()) + ")") << std::endl;
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
#if defined(WEBFRONT_TEST_FRONTEND)
        // CEF subprocesses have their own argument list and must dispatch before the mode is read.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
#endif
        const std::string_view mode      = argc > 1 ? argv[1] : "";
        const int              visibleMs = argc > 2 ? std::stoi(argv[2]) : 1500;
        if (mode == "browser")
            return validate<webfront::frontend::BrowserWithUILoopFrontend>(visibleMs);
#if defined(WEBFRONT_TEST_FRONTEND)
        if (mode == "embedded")
            return validate<WEBFRONT_TEST_FRONTEND>(visibleMs);
#endif
        std::cerr << "usage: native_integration_manual browser|embedded [dialog-ms]\n"
                     "       (embedded needs a build whose WEBFRONT_WEBTEST_FRONTEND is an enabled embedded frontend)\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
