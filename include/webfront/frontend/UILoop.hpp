/// @brief A minimal interface-thread loop for the system browser (ADR-020, #462).
///
/// The system browser owns its window and no toolkit loop runs in the application. `run(host, stop)` makes the calling
/// thread (the main thread, for the native dialogs) the interface thread: it attaches `host`, runs the platform's loop
/// until `stop.request()` and closes `host`. The loop is the Windows message pump (a message-only window, which a modal
/// dialog's own loop also dispatches), AppKit's run loop with `NSApplication` in the Accessory policy (no Dock icon) or the
/// GLib main loop. A platform whose runtime cannot be loaded makes `run` throw before attaching: the executor then stays
/// unavailable.
#pragma once

#include "UIExecutor.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
    #include "../system/WindowsCompat.hpp"
    #include <windows.h>
#elif defined(__APPLE__)
    #include <dlfcn.h>
#elif defined(__linux__)
    #include <dlfcn.h>
#endif

namespace webfront::frontend {

/// @brief Ends a loop started by `detail::uiloop::run`, from any thread, before or after it started. Reusable after run returns.
class UILoopStop {
public:
    /// Asks the loop to end; a request made before the loop starts ends it as soon as it starts.
    void request() noexcept {
        std::function<void()> wake;
        {
            std::lock_guard lock(mutex);
            requested = true;
            wake      = quit;
            if (wake)
                ++calls;  // disarm() waits for it: the quit function uses what the loop frees after disarming
        }
        if (!wake)
            return;
        struct Done {
            UILoopStop& self;
            ~Done() {
                std::lock_guard lock(self.mutex);
                --self.calls;
                self.callsDone.notify_all();
            }
        } done{*this};
        wake();
    }
    /// True once request() was called and until the next run starts.
    [[nodiscard]] bool requestedStop() const {
        std::lock_guard lock(mutex);
        return requested;
    }

    // For the loop implementation: installs how to end the loop; returns true when a request is already pending.
    bool arm(std::function<void()> function) {
        std::lock_guard lock(mutex);
        quit = std::move(function);
        return requested;
    }
    /// Waits for the request() calls in flight, then forgets the quit function and the request.
    void disarm() {
        std::unique_lock lock(mutex);
        quit = nullptr;  // no later request() copies it
        callsDone.wait(lock, [&] { return calls == 0; });
        requested = false;
    }

private:
    mutable std::mutex      mutex;
    std::condition_variable callsDone;
    unsigned                calls{0};
    bool                  requested{false};
    std::function<void()> quit;
};

namespace detail::uiloop {

/// Closes the host when the loop leaves, also by exception.
struct CloseGuard {
    UIExecutorHost& host;
    UILoopStop&     stop;
    ~CloseGuard() {
        host.close();
        stop.disarm();
    }
};

#if defined(_WIN32)

inline constexpr UINT drainMessage = WM_APP + 0x4C2;

struct LoopWindow {
    UIExecutorHost* host{};
    static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        if (message == drainMessage) {
            if (auto* self = reinterpret_cast<LoopWindow*>(GetWindowLongPtrW(handle, GWLP_USERDATA)))
                self->host->drain();
            return 0;
        }
        return DefWindowProcW(handle, message, wParam, lParam);
    }
};

inline void run(UIExecutorHost& host, UILoopStop& stop) {
    const wchar_t* className = L"WebFrontUILoop";
    WNDCLASSW      windowClass{};
    windowClass.lpfnWndProc   = LoopWindow::procedure;
    windowClass.hInstance     = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("UI loop: cannot register the message window class");
    LoopWindow context{&host};
    HWND       window = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    if (!window)
        throw std::runtime_error("UI loop: cannot create the message window");
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));
    struct Destroy {
        HWND window;
        ~Destroy() {
            DestroyWindow(window);
        }
    } destroy{window};
    const DWORD thread = GetCurrentThreadId();
    // A wake that fails (the window is gone) is dropped: closing abandons what is pending.
    host.attach([window] { PostMessageW(window, drainMessage, 0, 0); });
    // WM_QUIT belongs to the thread, not to the window: at most one is posted per opening (several requests are one), and
    // one left unconsumed (the loop left by an exception) is removed so that it cannot end the next opening.
    auto posted = std::make_shared<std::atomic<bool>>(false);
    struct PurgeQuit {
        std::shared_ptr<std::atomic<bool>> posted;
        ~PurgeQuit() {
            MSG left{};
            if (posted->load())
                while (PeekMessageW(&left, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) { }
        }
    } purge{posted};  // destroyed after the guard has disarmed: no request can post any more
    CloseGuard guard{host, stop};
    if (stop.arm([thread, posted] {
            if (!posted->exchange(true))
                PostThreadMessageW(thread, WM_QUIT, 0, 0);
        }))
        return;
    MSG message{};
    for (;;) {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
            throw std::runtime_error("UI loop: message loop failed");
        if (!result)
            return;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

#elif defined(__APPLE__)

// CoreFoundation and AppKit are loaded at run time: a consumer links nothing for the system browser's loop.
class CoreFoundation {
public:
    using Ref = const void*;
    struct SourceContext {
        long  version;
        void* info;
        void* retain;
        void* release;
        void* description;
        void* equal;
        void* hash;
        void (*schedule)(void*, Ref, Ref);
        void (*cancel)(void*, Ref, Ref);
        void (*perform)(void*);
    };

    Ref (*current)()                                  = nullptr;
    Ref (*sourceCreate)(void*, long, SourceContext*)  = nullptr;
    void (*addSource)(Ref, Ref, Ref)                  = nullptr;
    void (*removeSource)(Ref, Ref, Ref)               = nullptr;
    void (*signal)(Ref)                               = nullptr;
    void (*wakeUp)(Ref)                               = nullptr;
    int (*runInMode)(Ref, double, unsigned char)      = nullptr;
    void (*release)(Ref)                              = nullptr;
    Ref commonModes                                   = nullptr;
    Ref defaultMode                                   = nullptr;

    static const CoreFoundation& get() {
        static const CoreFoundation instance;
        return instance;
    }

private:
    template <typename T>
    static void bind(void* module, T& target, const char* name) {
        target = reinterpret_cast<T>(dlsym(module, name));
        if (!target)
            throw std::runtime_error(std::string("UI loop: missing CoreFoundation symbol ") + name);
    }
    static Ref constant(void* module, const char* name) {
        const auto* address = reinterpret_cast<const Ref*>(dlsym(module, name));
        if (!address)
            throw std::runtime_error(std::string("UI loop: missing CoreFoundation constant ") + name);
        return *address;
    }
    CoreFoundation() {
        // AppKit pulls CoreFoundation in; loading it also makes NSApplication available.
        void* module = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY);
        if (!module)
            throw std::runtime_error("UI loop: cannot load AppKit");
        bind(module, current, "CFRunLoopGetCurrent");
        bind(module, sourceCreate, "CFRunLoopSourceCreate");
        bind(module, addSource, "CFRunLoopAddSource");
        bind(module, removeSource, "CFRunLoopRemoveSource");
        bind(module, signal, "CFRunLoopSourceSignal");
        bind(module, wakeUp, "CFRunLoopWakeUp");
        bind(module, runInMode, "CFRunLoopRunInMode");
        bind(module, release, "CFRelease");
        commonModes = constant(module, "kCFRunLoopCommonModes");
        defaultMode = constant(module, "kCFRunLoopDefaultMode");
    }
};

// NSApplication in the Accessory policy (no Dock icon, no menu bar) so that panels can be shown. An application that
// already owns AppKit (another frontend, a host application) keeps its policy: only the default (Regular) one is changed.
// The Objective-C runtime is reached through dlsym, as CoreFoundation is: a consumer links nothing for this loop.
// The loop is an AppKit event pump (nextEvent / sendEvent), not a bare run loop: panels need their events dispatched.
class AppKitPump {
public:
    using Id       = void*;
    using Selector = void*;
    struct Point {
        double x, y;
    };

    bool ok{false};

    AppKitPump() {
        const auto getClass = reinterpret_cast<Id (*)(const char*)>(dlsym(RTLD_DEFAULT, "objc_getClass"));
        selector            = reinterpret_cast<Selector (*)(const char*)>(dlsym(RTLD_DEFAULT, "sel_registerName"));
        send                = dlsym(RTLD_DEFAULT, "objc_msgSend");
        poolPush            = reinterpret_cast<Id (*)()>(dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush"));
        poolPop             = reinterpret_cast<void (*)(Id)>(dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop"));
        const auto mode     = reinterpret_cast<Id*>(dlsym(RTLD_DEFAULT, "NSDefaultRunLoopMode"));
        if (!getClass || !selector || !send || !poolPush || !poolPop || !mode)
            return;
        defaultMode      = *mode;
        const auto appClass = getClass("NSApplication");
        eventClass          = getClass("NSEvent");
        dateClass           = getClass("NSDate");
        if (!appClass || !eventClass || !dateClass)
            return;
        application = call<Id>(appClass, "sharedApplication");
        if (!application)
            return;
        if (call<long>(application, "activationPolicy") == 0)
            call<unsigned char>(application, "setActivationPolicy:", 1L);  // Accessory
        call<void>(application, "finishLaunching");
        ok = true;
    }

    /// Thread-safe: makes next() return, so that the loop reads its flags again.
    void wake() const {
        const auto pool  = poolPush();
        const auto event = call<Id>(eventClass, "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:",
                                    15UL /* NSEventTypeApplicationDefined */, Point{0, 0}, 0UL, 0.0, 0L, Id{}, short{0}, 0L, 0L);
        if (event)
            call<void>(application, "postEvent:atStart:", event, static_cast<signed char>(1));
        poolPop(pool);
    }

    /// One turn of the pump: waits for an event (the run loop's sources, such as the executor's, run meanwhile) and dispatches it.
    void next() const {
        const auto pool  = poolPush();
        const auto until = call<Id>(dateClass, "distantFuture");
        const auto event = call<Id>(application, "nextEventMatchingMask:untilDate:inMode:dequeue:", ~0UL, until, defaultMode,
                                    static_cast<signed char>(1));
        if (event)
            call<void>(application, "sendEvent:", event);
        poolPop(pool);
    }

private:
    template <typename R, typename... Args>
    R call(Id target, const char* name, Args... args) const {
        return reinterpret_cast<R (*)(Id, Selector, Args...)>(send)(target, selector(name), args...);
    }

    Selector (*selector)(const char*) = nullptr;
    void*  send{};
    Id     (*poolPush)()   = nullptr;
    void   (*poolPop)(Id)  = nullptr;
    Id     application{};
    Id     eventClass{};
    Id     dateClass{};
    Id     defaultMode{};
};

inline void run(UIExecutorHost& host, UILoopStop& stop) {
    const auto& cf = CoreFoundation::get();
    const AppKitPump pump;
    CoreFoundation::SourceContext context{};
    context.info    = &host;
    context.perform = +[](void* info) { static_cast<UIExecutorHost*>(info)->drain(); };
    const auto source = cf.sourceCreate(nullptr, 0, &context);
    if (!source)
        throw std::runtime_error("UI loop: cannot create the run loop source");
    const auto loop = cf.current();
    cf.addSource(loop, source, cf.commonModes);
    struct Remove {
        const CoreFoundation& cf;
        CoreFoundation::Ref   loop;
        CoreFoundation::Ref   source;
        ~Remove() {
            cf.removeSource(loop, source, cf.commonModes);
            cf.release(source);
        }
    } remove{cf, loop, source};
    const auto signalLoop = [&cf, &pump, loop, source] {
        cf.signal(source);
        cf.wakeUp(loop);
        if (pump.ok)
            pump.wake();  // nextEvent only returns for an event: the source ran, the loop must also look at its flags
    };
    host.attach(signalLoop);
    CloseGuard guard{host, stop};
    // A stop request signals the same way: the loop reads the flag again after each turn, so a request made between the
    // check and the wait is not lost (it posted an event).
    if (stop.arm(signalLoop))
        return;
    if (pump.ok) {
        while (!stop.requestedStop())
            pump.next();
        return;
    }
    while (!stop.requestedStop())  // no Objective-C runtime: sources only
        cf.runInMode(cf.defaultMode, 1.0e9, 1);
}

#elif defined(__linux__)

// GLib is loaded at run time, like the WebKitGTK adapter loads its toolkit.
class GLib {
public:
    using Object = void*;
    Object (*contextDefault)()                                           = nullptr;
    Object (*loopNew)(Object, int)                                       = nullptr;
    void (*loopRun)(Object)                                              = nullptr;
    void (*loopQuit)(Object)                                             = nullptr;
    void (*loopUnref)(Object)                                            = nullptr;
    Object (*loopRef)(Object)                                            = nullptr;
    unsigned (*idleAddFull)(int, int (*)(void*), void*, void (*)(void*)) = nullptr;
    void (*contextWakeup)(Object)                                        = nullptr;

    static const GLib& get() {
        static const GLib instance;
        return instance;
    }

private:
    template <typename T>
    static void bind(void* module, T& target, const char* name) {
        target = reinterpret_cast<T>(dlsym(module, name));
        if (!target)
            throw std::runtime_error(std::string("UI loop: missing GLib symbol ") + name);
    }
    GLib() {
        void* module = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!module)
            throw std::runtime_error("UI loop: cannot load libglib-2.0.so.0; install GLib or do without the system browser's UI loop");
        bind(module, contextDefault, "g_main_context_default");
        bind(module, loopNew, "g_main_loop_new");
        bind(module, loopRun, "g_main_loop_run");
        bind(module, loopQuit, "g_main_loop_quit");
        bind(module, loopUnref, "g_main_loop_unref");
        bind(module, loopRef, "g_main_loop_ref");
        bind(module, idleAddFull, "g_idle_add_full");
        bind(module, contextWakeup, "g_main_context_wakeup");
    }
};

inline void run(UIExecutorHost& host, UILoopStop& stop) {
    const auto& glib = GLib::get();
    const auto  loop = glib.loopNew(glib.contextDefault(), 0);
    if (!loop)
        throw std::runtime_error("UI loop: cannot create the GLib main loop");
    struct Unref {
        const GLib& glib;
        GLib::Object loop;
        ~Unref() {
            glib.loopUnref(loop);
        }
    } unref{glib, loop};
    constexpr int defaultIdle = 200;  // G_PRIORITY_DEFAULT_IDLE
    host.attach([&glib, &host] {
        glib.idleAddFull(
            defaultIdle,
            +[](void* data) -> int {
                static_cast<UIExecutorHost*>(data)->drain();
                return 0;  // G_SOURCE_REMOVE
            },
            &host,
            nullptr);
    });
    CloseGuard guard{host, stop};
    // Quitting from an idle callback happens inside the loop: a request before g_main_loop_run is not lost. Each pending
    // source owns a reference to the loop (released when the source is destroyed, run or not), so one that is still queued
    // when run() returns, from a second request during the exit, never reaches a freed loop.
    static const auto quitLoop = +[](void* data) -> int {
        GLib::get().loopQuit(data);
        return 0;
    };
    static const auto releaseLoop = +[](void* data) { GLib::get().loopUnref(data); };
    if (stop.arm([&glib, loop] { glib.idleAddFull(defaultIdle, quitLoop, glib.loopRef(loop), releaseLoop); }))
        return;
    glib.loopRun(loop);
}

#else

inline void run(UIExecutorHost&, UILoopStop&) {
    throw std::runtime_error("UI loop: this platform has no interface-thread loop");
}

#endif

}  // namespace detail::uiloop
}  // namespace webfront::frontend
