#pragma once

#include "../ContentPolicy.hpp"
#include "Action.hpp"
#include "Capture.hpp"
#include "Downloads.hpp"
#include "ScriptDialogs.hpp"
#include "Url.hpp"
#include "Window.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <type_traits>

namespace webfront::frontend {

/** @brief Per-opening options for the optional macOS frontend. */
struct WKWebViewOptions {
    /** @brief The initial document must finish loading within this interval; no bridge connection is required. */
    std::chrono::milliseconds startupTimeout{30000};
    ContentPolicy             contentPolicy{};
    WindowOptions             window{};
    /** @brief Keep the native window hidden and avoid activating the application. A WindowServer is still required. */
    bool                      visible{true};
    /** @brief Test injection: answers `<input type="file">` without a dialog (an empty list cancels). Unset shows the system panel. */
    std::function<std::vector<std::string>()> chooseFiles{};
    /** @brief Test injection: a non-empty folder receives downloads without a save panel. */
    std::filesystem::path downloadDirectory{};
    /** @brief Test injection: called with the destination of each finished download. */
    std::function<void(const std::string&)> downloadFinished{};
    /** @brief Test injection: answers `alert`, `confirm` and `prompt` without a box. Unset shows an NSAlert sheet on the window.
     *  `beforeunload` is not handled by WKWebView: the public WKUIDelegate has no method for it, and it does not reach the confirm
     *  method. A page that relies on it can lose unsaved changes on navigation or close; WebFront does not confirm it. */
    ScriptDialogHandler answerScriptDialog{};
};

}  // namespace webfront::frontend

#if defined(WEBFRONT_EMBED_WKWEBVIEW) && defined(__APPLE__)
    #include <bit>
    #include <exception>
    #include <functional>
    #include <future>
    #include <memory>
#include <mutex>
    #include <string>
    #include <utility>
    #include <vector>

    #include <Block.h>
    #include <CoreGraphics/CGGeometry.h>
    #include <dispatch/dispatch.h>
    #include <objc/message.h>
    #include <objc/runtime.h>
    #include <pthread.h>

namespace webfront::frontend::detail::wkwebview {

// Use the public Objective-C runtime from C++: no .mm translation unit, ARC, or Cocoa Objective-C headers.
// Clang's Blocks extension supplies the typed WebKit decision-handler ABI.
template <typename Result = id, typename... Args>
Result send(id object, const char* selector, Args... args) {
    return reinterpret_cast<Result (*)(id, SEL, Args...)>(objc_msgSend)(object, sel_registerName(selector), args...);
}

inline id type(const char* name) {
    auto result = reinterpret_cast<id>(objc_getClass(name));
    if (!result)
        throw std::runtime_error(std::string("WKWebView: unavailable Cocoa class ") + name);
    return result;
}

class Object {
public:
    explicit Object(id value = nullptr) : value(value) { }
    Object(const Object&)            = delete;
    Object& operator=(const Object&) = delete;
    ~Object() {
        send<void>(value, "release");
    }
    id get() const {
        return value;
    }

private:
    id value;
};

inline id text(std::string_view value) {
    return send(send(type("NSString"), "alloc"), "initWithBytes:length:encoding:", value.data(), value.size(), 4UL);
}

inline std::string utf8(id value) {
    const auto bytes = send<const char*>(value, "UTF8String");
    return bytes ? bytes : "";
}

inline void requireMainThread() {
    if (!pthread_main_np())
        throw std::runtime_error("WKWebView: construct the application and call show()/open() on the main thread");
}

inline void initialize() {
    requireMainThread();
    struct Version {
        long major, minor, patch;
    };
    Object pool(send(type("NSAutoreleasePool"), "new"));
    if (!send<BOOL>(send(type("NSProcessInfo"), "processInfo"), "isOperatingSystemAtLeastVersion:", Version{13, 3, 0}))
        throw std::runtime_error("WKWebView: macOS 13.3 or newer is required");
}

inline long applicationActivationPolicy() {
    Object agentKey(text("LSUIElement"));
    const auto agent = send(send(type("NSBundle"), "mainBundle"), "objectForInfoDictionaryKey:", agentKey.get());
    return send<BOOL>(agent, "boolValue") ? 1L : 0L;  // Accessory for agent bundles, Regular otherwise.
}

struct MenuCommand {
    const char* title;
    const char* action;
    const char* key;
};

struct Session;

inline Session* session(id object) noexcept {
    void* pointer = nullptr;
    object_getInstanceVariable(object, "webfrontSession", &pointer);
    return static_cast<Session*>(pointer);
}

inline void attach(id object, Session* pointer) noexcept {
    object_setInstanceVariable(object, "webfrontSession", pointer);
}

inline void addSessionIvar(Class cls) {
    if (!class_addIvar(cls, "webfrontSession", sizeof(Session*), static_cast<std::uint8_t>(std::countr_zero(alignof(Session*))), "^v"))
        throw std::runtime_error("WKWebView: cannot register session storage");
}

template <typename Function>
void addMethod(Class cls, const char* selector, Function function, const char* signature) {
    if (!class_addMethod(cls, sel_registerName(selector), reinterpret_cast<IMP>(function), signature))
        throw std::runtime_error(std::string("WKWebView: cannot register ") + selector);
}

inline Class delegateClass();
inline Class viewClass();
inline bool  opening = false;  // AppKit sessions are main-thread-only and cannot nest.

template <typename Configure>
Class registerClass(const char* name, Class base, Configure configure) {
    if (auto existing = objc_lookUpClass(name))
        return existing;
    auto created = objc_allocateClassPair(base, name, 0);
    if (!created) {
        if (auto existing = objc_lookUpClass(name))
            return existing;
        throw std::runtime_error(std::string("WKWebView: cannot register ") + name);
    }
    try {
        addSessionIvar(created);
        configure(created);
    } catch (...) {
        objc_disposeClassPair(created);
        throw;
    }
    objc_registerClassPair(created);
    return created;
}

inline bool successfulHttpStatus(long status) {
    return status >= 200 && status < 400;
}

/// The view as seen by capture requests: read and cleared only on the main thread, so a request that outlives the
/// window finds it null.
struct CaptureJob;
struct CaptureTarget {
    id                                       view = nullptr;
    PendingCaptures<CaptureJob> pending;  // Requests nobody has answered yet, queued or in flight.
};

struct CaptureJob {
    std::shared_ptr<CaptureTarget> target;
    std::promise<CaptureResult>    done;
};

inline void answer(CaptureJob& job, CaptureResult result) noexcept {
    try {
        job.done.set_value(std::move(result));
    } catch (...) { }  // Already answered: a closing window may have answered before the engine did.
}

/// The window is going away: no pending request waits for an answer that will never come.
inline void abandonCaptures(CaptureTarget& target) noexcept {
    target.view = nullptr;
    for (const auto& job : target.pending.close())
        answer(*job, std::unexpected("the window is closed"));
}


// The PNG of an NSImage through NSBitmapImageRep (NSBitmapImageFileTypePNG = 4).
inline CaptureResult pngBytes(id image) {
    const auto tiff = send(image, "TIFFRepresentation");
    if (!tiff)
        return std::unexpected("cannot read the snapshot pixels");
    const auto representation = send(type("NSBitmapImageRep"), "imageRepWithData:", tiff);
    if (!representation)
        return std::unexpected("cannot decode the snapshot");
    const auto png = send(representation, "representationUsingType:properties:", 4UL, send(type("NSDictionary"), "dictionary"));
    if (!png)
        return std::unexpected("cannot encode the snapshot as PNG");
    const auto bytes = send<const std::uint8_t*>(png, "bytes");
    const auto size  = send<unsigned long>(png, "length");
    return std::vector<std::uint8_t>(bytes, bytes + size);
}

// Runs on the main thread, where AppKit sessions live.
inline void startCapture(void* data) noexcept {
    std::unique_ptr<std::shared_ptr<CaptureJob>> owner{static_cast<std::shared_ptr<CaptureJob>*>(data)};
    const auto                                   job = *owner;
    try {
        if (!job->target->view) {
            answer(*job, std::unexpected("the window is closed"));
            return;
        }
        auto handler = ^(id image, id error) {
            try {
                job->target->pending.untrack(job);
                if (!image) {
                    const auto reason = error ? utf8(send(error, "localizedDescription")) : std::string("the engine returned no image");
                    answer(*job, std::unexpected("snapshot failed: " + reason));
                } else {
                    answer(*job, pngBytes(image));
                }
            } catch (...) {
                try {
                    job->done.set_exception(std::current_exception());
                } catch (...) { }
            }
        };
        // A nil configuration snapshots the visible bounds of the view.
        // The engine keeps its own copy until the handler ran; the call never depends on the stack block surviving it.
        const auto heapHandler = Block_copy(handler);
        try {
            send<void>(job->target->view, "takeSnapshotWithConfiguration:completionHandler:", static_cast<id>(nullptr), heapHandler);
        } catch (...) {
            job->target->pending.untrack(job);
            Block_release(heapHandler);
            throw;
        }
        Block_release(heapHandler);
    } catch (...) {
        try {
            job->done.set_exception(std::current_exception());
        } catch (...) { }
    }
}

inline CaptureProvider captureProvider(std::shared_ptr<CaptureTarget> target) {
    return [target = std::move(target)](std::chrono::milliseconds timeout) -> CaptureResult {
        auto job    = std::make_shared<CaptureJob>(CaptureJob{target, {}});
        auto future = job->done.get_future();
        if (!target->pending.track(job))
            return std::unexpected("the window is closed");
        dispatch_async_f(dispatch_get_main_queue(), new std::shared_ptr<CaptureJob>(job), startCapture);
        const auto answered = future.wait_for(timeout) == std::future_status::ready;
        target->pending.untrack(job);  // Whatever the outcome, a finished request is not kept until the window closes.
        if (!answered) {
            return std::unexpected("the window did not answer within the capture timeout");
        }
        return future.get();
    };
}

struct Session {
    explicit Session(std::string url, WKWebViewOptions options = {})
        : url(std::move(url)), options(std::move(options)), contentPolicy(this->url, this->options.contentPolicy) {
        if (this->options.startupTimeout.count() <= 0)
            throw std::invalid_argument("WKWebView: startupTimeout must be positive");
    }

    std::string                          url;
    WKWebViewOptions                     options;
    webfront::detail::BoundContentPolicy contentPolicy;
    std::function<void()>                navigationCancelled;
    struct PendingDownload {
        std::unique_ptr<frontend::detail::DownloadStaging> staging;
        std::filesystem::path target;
        bool reserved;
        bool replace;
    };
    frontend::detail::DownloadReservations reservations;
    std::map<id, PendingDownload>        downloadDestinations;  // WKDownload -> its paths, until it ends.
    /// What the answers of a system panel hold instead of `this`: AppKit may deliver one after the session is gone.
    std::shared_ptr<Session*>            alive = std::make_shared<Session*>(this);
    id                                   app = nullptr, window = nullptr, view = nullptr, delegate = nullptr, timer = nullptr;
    id                                   previousDelegate = nullptr, previousMenu = nullptr;
    long                                 previousActivationPolicy = 0;
    bool                                 installed = false, running = false, closed = false, loaded = false;
    bool                                 receivedResponse = false, committed = false;
    std::exception_ptr                   error;
    std::shared_ptr<CaptureTarget>       captureTarget = std::make_shared<CaptureTarget>();
    CaptureRegistration                  captureRegistration;

    ~Session() {
        *alive = nullptr;
        endSheets();  // Each open panel answers "cancel" now, while the view still exists to receive it.
        abandonDownloads();
        captureRegistration = {};
        abandonCaptures(*captureTarget);
        if (timer) {
            send<void>(timer, "invalidate");
            send<void>(timer, "release");
        }
        if (view) {
            send<void>(view, "setNavigationDelegate:", static_cast<id>(nullptr));
            send<void>(view, "setUIDelegate:", static_cast<id>(nullptr));
            send<void>(view, "stopLoading");
            attach(view, nullptr);
        }
        if (window) {
            send<void>(window, "setDelegate:", static_cast<id>(nullptr));
            send<void>(window, "close");
            send<void>(window, "release");
        }
        send<void>(view, "release");
        if (installed) {
            send<void>(app, "setDelegate:", previousDelegate);
            if (previousMenu) {
                send<void>(app, "setMainMenu:", previousMenu);
            } else {
                // AppKit ignores setMainMenu:nil. Install an empty menu so our Quit item
                // cannot survive its delegate, including a close before the first run().
                Object emptyMenu(send(type("NSMenu"), "new"));
                send<void>(app, "setMainMenu:", emptyMenu.get());
            }
            send<BOOL>(app, "setActivationPolicy:", previousActivationPolicy);
        }
        send<void>(previousMenu, "release");
        send<void>(previousDelegate, "release");
        if (delegate) {
            attach(delegate, nullptr);
            send<void>(delegate, "release");
        }
    }

    /// Dismisses the panels attached to the window: their completion handlers run, so WebKit gets its answers.
    void endSheets() noexcept {
        if (!window)
            return;
        const auto sheets = send(window, "sheets");
        const auto count  = send<unsigned long>(sheets, "count");
        for (auto index = count; index > 0; --index)
            send<void>(window, "endSheet:", send(sheets, "objectAtIndex:", index - 1));
    }

    /// Cancels the downloads still running and removes the incomplete files they were writing.
    void abandonDownloads() noexcept {
        auto pending = std::exchange(downloadDestinations, {});  // Their failure callbacks must find nothing to settle.
        for (const auto& [download, paths] : pending) {
            send<void>(download, "cancel:", static_cast<id>(nullptr));
            if (paths.reserved)
                reservations.release(paths.target);
        }
    }

    void close() noexcept {
        closed = true;
        if (!running)
            return;
        send<void>(app, "stop:", static_cast<id>(nullptr));
        // stop: takes effect after an event. A close from a WebKit callback/timer must wake run.
        auto event = send(type("NSEvent"),
                          "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:",
                          15UL,
                          CGPoint{0, 0},
                          0UL,
                          0.0,
                          0L,
                          static_cast<id>(nullptr),
                          static_cast<short>(0),
                          0L,
                          0L);
        send<void>(app, "postEvent:atStart:", event, static_cast<BOOL>(true));
    }

    template <typename Function>
    void guard(Function&& function) noexcept {
        try {
            function();
        } catch (...) {
            if (!error)
                error = std::current_exception();
            close();
        }
    }

    void fail(std::string message) noexcept {
        guard([&] {
            throw std::runtime_error("WKWebView: " + message);
        });
    }

    void rejectNavigation(std::string_view diagnostic) {
        if (!committed)
            fail(std::string(diagnostic));
        else if (navigationCancelled)
            navigationCancelled();
    }

    void addMenuItem(id menu, const MenuCommand& command, id target = nullptr) {
        Object label(text(command.title));
        Object shortcut(text(command.key));
        Object item(
            send(send(type("NSMenuItem"), "alloc"), "initWithTitle:action:keyEquivalent:", label.get(), sel_registerName(command.action), shortcut.get()));
        send<void>(item.get(), "setTarget:", target);
        send<void>(menu, "addItem:", item.get());
    }

    void installMenu() {
        Object menu(send(type("NSMenu"), "new"));
        for (const auto title : {"Application", "Edit"}) {
            Object label(text(title));
            Object parent(send(type("NSMenuItem"), "new"));
            send<void>(parent.get(), "setTitle:", label.get());
            Object submenu(send(send(type("NSMenu"), "alloc"), "initWithTitle:", label.get()));
            if (std::string_view(title) == "Application") {
                addMenuItem(submenu.get(), {"Quit", "webfrontQuit:", "q"}, delegate);
            } else {
                addMenuItem(submenu.get(), {"Cut", "cut:", "x"});
                addMenuItem(submenu.get(), {"Copy", "copy:", "c"});
                addMenuItem(submenu.get(), {"Paste", "paste:", "v"});
                addMenuItem(submenu.get(), {"Select All", "selectAll:", "a"});
            }
            send<void>(parent.get(), "setSubmenu:", submenu.get());
            send<void>(menu.get(), "addItem:", parent.get());
        }
        send<void>(app, "setMainMenu:", menu.get());
    }

    void create() {
        app = send(type("NSApplication"), "sharedApplication");
        if (send<BOOL>(app, "isRunning"))
            throw std::runtime_error("WKWebView: open() owns the AppKit event loop; call it outside an existing NSApplication run loop");
        previousDelegate         = send(send(app, "delegate"), "retain");
        previousMenu             = send(send(app, "mainMenu"), "retain");
        previousActivationPolicy = send<long>(app, "activationPolicy");
        delegate                 = send(reinterpret_cast<id>(delegateClass()), "new");
        attach(delegate, this);
        installed = true;
        send<void>(app, "setDelegate:", delegate);
        send<BOOL>(app, "setActivationPolicy:", options.visible ? applicationActivationPolicy() : 2L);
        installMenu();
        send<void>(app, "finishLaunching");

        Object configuration(send(type("WKWebViewConfiguration"), "new"));
        send<void>(configuration.get(), "setWebsiteDataStore:", send(type("WKWebsiteDataStore"), "nonPersistentDataStore"));
        // Hidden content otherwise suspends once its load event completes. Use the public macOS 14+ preference
        // so asynchronous bridge/Worker work and close requests keep progressing without a visible native window.
        if (!options.visible) {
            const auto preferences = send(configuration.get(), "preferences");
            const auto selector = sel_registerName("setInactiveSchedulingPolicy:");
            if (!send<BOOL>(preferences, "respondsToSelector:", selector))
                throw std::runtime_error("WKWebView: hidden sessions require macOS 14 or later (inactiveSchedulingPolicy)");
            send<void>(preferences, "setInactiveSchedulingPolicy:", 2L); // WKInactiveSchedulingPolicyNone
        }
        // Deliver script-created popup attempts to the delegates, which always cancel unmanaged views.
        send<void>(send(configuration.get(), "preferences"), "setJavaScriptCanOpenWindowsAutomatically:", static_cast<BOOL>(true));
        const auto   windowOptions = frontend::detail::resolveWindow(options.window);
        const CGRect bounds{
            {                                 0,                                   0},
            {static_cast<CGFloat>(windowOptions.width), static_cast<CGFloat>(windowOptions.height)}
        };
        view = send(send(reinterpret_cast<id>(viewClass()), "alloc"), "initWithFrame:configuration:", bounds, configuration.get());
        if (!view)
            throw std::runtime_error("WKWebView: cannot create web view");
        attach(view, this);
        captureTarget->view = view;
        captureTarget->pending.open();
        captureRegistration = CaptureRegistration{captureProvider(captureTarget)};
        send<void>(view, "setAutoresizingMask:", 18UL);  // NSViewWidthSizable | NSViewHeightSizable
        send<void>(view, "setInspectable:", static_cast<BOOL>(options.contentPolicy.devToolsEnabled));
        send<void>(view, "setNavigationDelegate:", delegate);
        send<void>(view, "setUIDelegate:", delegate);
        window = send(send(type("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", bounds, 15UL, 2UL, static_cast<BOOL>(false));
        if (!window)
            throw std::runtime_error("WKWebView: cannot create window (a WindowServer session is required)");
        send<void>(window, "setReleasedWhenClosed:", static_cast<BOOL>(false));
        send<void>(window, "setDelegate:", delegate);
        Object title(text(windowOptions.title));
        send<void>(window, "setTitle:", title.get());
        send<void>(window, "setContentView:", view);
        send<BOOL>(window, "makeFirstResponder:", view);
        send<void>(window, "center");
        if (options.visible) {
            send<void>(window, "makeKeyAndOrderFront:", static_cast<id>(nullptr));
            send<void>(app, "activateIgnoringOtherApps:", static_cast<BOOL>(true));
        }
    }

    void start() {
        timer = send(send(type("NSTimer"),
                          "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
                          std::chrono::duration<double>(options.startupTimeout).count(),
                          delegate,
                          sel_registerName("webfrontTimeout:"),
                          static_cast<id>(nullptr),
                          static_cast<BOOL>(false)),
                     "retain");
        Object urlText(text(url));
        auto   nativeUrl = send(type("NSURL"), "URLWithString:", urlText.get());
        if (!nativeUrl)
            throw std::runtime_error("WKWebView: invalid application URL");
        if (!send(view, "loadRequest:", send(type("NSURLRequest"), "requestWithURL:", nativeUrl)))
            fail("initial navigation failed to start");
    }

    void run() {
        if (!closed) {
            running = true;
            send<void>(app, "run");
            running = false;
        }
        if (error)
            std::rethrow_exception(error);
    }

    void navigationAction(id action, void (^decision)(long)) noexcept {
        long policy = 0;  // WKNavigationActionPolicyCancel
        guard([&] {
            const auto frame = send(action, "targetFrame");
            if (!frame)
                return;  // No public WebKit user-activation flag: conservatively block every popup.
            if (!send<BOOL>(frame, "isMainFrame")) {
                policy = 1;
                return;
            }
            const auto destination = utf8(send(send(send(action, "request"), "URL"), "absoluteString"));
            if (contentPolicy.allowsNavigation(destination)) {
                policy = 1;  // WKNavigationActionPolicyAllow
                // <a download> and generated Blobs: WKNavigationActionPolicyDownload (macOS 11.3).
                if (send<BOOL>(action, "respondsToSelector:", sel_registerName("shouldPerformDownload")) && send<BOOL>(action, "shouldPerformDownload"))
                    policy = 2;
            } else
                rejectNavigation("initial navigation was denied by contentPolicy");
        });
        decision(policy);  // Exactly once, including after a C++ exception.
    }

    /// The address a frame is judged by. A frame the page creates without a document of its own (no `src`, or `srcdoc`)
    /// is `about:blank` / `about:srcdoc` and inherits its creator's origin, which is what the policy has to see.
    static std::string effectiveFrameUrl(id frame) {
        auto address = utf8(send(send(send(frame, "request"), "URL"), "absoluteString"));
        if (!address.empty() && !address.starts_with("about:"))
            return address;
        const auto origin   = send(frame, "securityOrigin");
        const auto protocol = utf8(send(origin, "protocol"));
        const auto host     = utf8(send(origin, "host"));
        if (protocol.empty() || host.empty())
            return address;  // An opaque origin (sandboxed frame): not the application's.
        const auto port = send<long>(origin, "port");
        return protocol + "://" + host + (port > 0 ? ":" + std::to_string(port) : "") + "/";
    }

    /// <input type="file">: the system panel as a sheet of the window, or the test injection. The handler runs exactly once.
    void chooseFilesForPage(id parameters, id frame, void (^completion)(id)) noexcept {
        id   result   = nullptr;  // NSArray<NSURL*>*; nil cancels.
        bool deferred = false;
        guard([&] {
            // Only the application's own origin may open a chooser, not every origin that navigation admits.
            if (!contentPolicy.isApplicationOrigin(effectiveFrameUrl(frame)))
                return;
            if (options.chooseFiles) {
                const auto files = options.chooseFiles();
                if (files.empty())
                    return;
                const auto urls = send(type("NSMutableArray"), "array");
                for (const auto& file : files) {
                    Object path(text(file));
                    send<void>(urls, "addObject:", send(type("NSURL"), "fileURLWithPath:", path.get()));
                }
                result = urls;
                return;
            }
            const auto panel = send(type("NSOpenPanel"), "openPanel");
            send<void>(panel, "setAllowsMultipleSelection:", send<BOOL>(parameters, "allowsMultipleSelection"));
            // <input webkitdirectory> chooses a folder, and only a folder.
            const auto directories = send<BOOL>(parameters, "allowsDirectories");
            send<void>(panel, "setCanChooseDirectories:", directories);
            send<void>(panel, "setCanChooseFiles:", static_cast<BOOL>(!directories));
            auto handler  = Block_copy(completion);
            bool retained = false;
            try {
                send<void>(panel, "retain");
                retained = true;
                void (^finished)(long) = ^(long response) {
                    handler(response == 1 ? send(panel, "URLs") : nullptr);  // NSModalResponseOK
                    send<void>(panel, "release");
                    Block_release(handler);
                };
                send<void>(panel, "beginSheetModalForWindow:completionHandler:", window, finished);
            } catch (...) {
                if (retained)
                    send<void>(panel, "release");
                Block_release(handler);  // The panel never took the handler: `completion` answers below.
                throw;
            }
            deferred = true;
        });
        if (!deferred)
            completion(result);
    }

    /// `alert`, `confirm` and `prompt` (WKUIDelegate): an NSAlert sheet of the window, or the test injection. `answer` runs exactly once,
    /// with a dismissed answer when the page's origin is not the application's, when the window closes, or when the box cannot be shown.
    void runScriptDialog(ScriptDialog dialog, id frame, std::function<void(ScriptDialogAnswer)> answer) noexcept {
        ScriptDialogAnswer result;  // Dismissed.
        bool               deferred = false;
        guard([&] {
            if (!contentPolicy.isApplicationOrigin(effectiveFrameUrl(frame)))
                return;
            if (options.answerScriptDialog) {
                result = options.answerScriptDialog(dialog);
                return;
            }
            const id alert    = send(send(type("NSAlert"), "alloc"), "init");  // Owned until the sheet answers.
            id       field    = nullptr;
            bool     retained = true;
            try {
                {
                    Object message(text(dialog.message));
                    send<void>(alert, "setMessageText:", message.get());
                }
                const bool confirming = dialog.kind != ScriptDialog::Kind::alert;
                {
                    Object accept(text("OK"));
                    send<void>(alert, "addButtonWithTitle:", accept.get());  // NSAlertFirstButtonReturn (1000).
                }
                if (confirming) {
                    Object cancel(text("Cancel"));
                    send<void>(alert, "addButtonWithTitle:", cancel.get());
                }
                if (dialog.kind == ScriptDialog::Kind::prompt) {
                    field = send(send(type("NSTextField"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {300, 24}});
                    Object value(text(dialog.defaultText));
                    send<void>(field, "setStringValue:", value.get());
                    send<void>(alert, "setAccessoryView:", field);
                    send<void>(alert, "layout");
                    send<void>(send(alert, "window"), "setInitialFirstResponder:", field);
                }
                auto shared = std::make_shared<std::function<void(ScriptDialogAnswer)>>(std::move(answer));
                void (^finished)(long) = ^(long response) {
                    ScriptDialogAnswer reply;
                    reply.accepted = response == 1000;  // endSheet: at window close reports NSModalResponseCancel.
                    if (reply.accepted && field)
                        reply.text = utf8(send(field, "stringValue"));
                    (*shared)(std::move(reply));
                    if (field)
                        send<void>(field, "release");
                    send<void>(alert, "release");
                };
                try {
                    send<void>(alert, "beginSheetModalForWindow:completionHandler:", window, finished);
                } catch (...) {
                    // The call failed to dispatch, so the block never runs and never releases: this function still owns
                    // the alert and the field (released below) and answers dismissed.
                    answer = std::move(*shared);
                    throw;
                }
                retained = false;  // The sheet started: the block owns the alert and the field until it answers.
            } catch (...) {
                if (retained) {
                    if (field)
                        send<void>(field, "release");
                    send<void>(alert, "release");
                }
                throw;
            }
            deferred = true;
        });
        if (!deferred)
            answer(std::move(result));
    }

    /// A WKDownload needs a destination that does not exist: the test folder, or the user's choice in a save panel.
    void chooseDownloadDestination(id download, const std::string& suggested, void (^completion)(id)) noexcept {
        id   result   = nullptr;  // NSURL*; nil cancels the download.
        bool deferred = false;
        guard([&] {
            if (!options.downloadDirectory.empty()) {
                std::filesystem::create_directories(options.downloadDirectory);  // WebKit needs the folder to exist.
                std::filesystem::path destination;
                try {
                    destination = reservations.reserve(options.downloadDirectory, suggested);
                } catch (const std::filesystem::filesystem_error&) {
                    return;  // No candidate is free: nil cancels this download without stopping the session.
                }
                std::unique_ptr<frontend::detail::DownloadStaging> staging;
                try {
                    staging = std::make_unique<frontend::detail::DownloadStaging>(destination.parent_path());
                } catch (...) {
                    reservations.release(destination);
                    throw;
                }
                const auto temporary = staging->path();
                downloadDestinations[download] = {std::move(staging), destination, true, false};
                Object path(text(temporary.string()));
                result = send(type("NSURL"), "fileURLWithPath:", path.get());
                return;
            }
            const auto panel = send(type("NSSavePanel"), "savePanel");
            Object     name(text(std::filesystem::path(suggested).filename().string()));
            send<void>(panel, "setNameFieldStringValue:", name.get());
            auto     handler  = Block_copy(completion);
            bool     retained = false;
            auto     token    = alive;     // Not `this`: the window may close, and the session end, while the panel is open.
            const id key      = download;  // By value: the panel answers after this lambda and its references are gone.
            try {
                send<void>(panel, "retain");
                retained = true;
                void (^finished)(long) = ^(long response) {
                    id chosen = response == 1 ? send(panel, "URL") : nullptr;  // NSModalResponseOK
                    if (chosen && !*token)
                        chosen = nullptr;  // The session is gone: nobody could move the file afterwards.
                    try {
                        if (chosen) {
                            const std::filesystem::path target = utf8(send(chosen, "path"));
                            std::error_code             code;
                            const bool                  replace = std::filesystem::exists(target, code);
                            if (code)
                                throw std::filesystem::filesystem_error("Cannot inspect download target", target, code);
                            auto& sessionReservations = (*token)->reservations;
                            if (!sessionReservations.reserveExact(target))
                                throw std::filesystem::filesystem_error("Another download is writing this target", target,
                                                                        std::make_error_code(std::errc::file_exists));
                            try {
                                auto       staging   = std::make_unique<frontend::detail::DownloadStaging>(target.parent_path());
                                const auto temporary = staging->path();
                                Object     path(text(temporary.string()));
                                chosen = send(type("NSURL"), "fileURLWithPath:", path.get());
                                (*token)->downloadDestinations[key] = {std::move(staging), target, true, replace};
                            } catch (...) {
                                sessionReservations.release(target);
                                throw;
                            }
                        }
                    } catch (const std::filesystem::filesystem_error&) {
                        chosen = nullptr;  // No staging, or the target is taken: answer WebKit once, without crossing the AppKit block.
                    }
                    handler(chosen);
                    send<void>(panel, "release");
                    Block_release(handler);
                };
                send<void>(panel, "beginSheetModalForWindow:completionHandler:", window, finished);
            } catch (...) {
                if (retained)
                    send<void>(panel, "release");
                Block_release(handler);  // The panel never took the handler: `completion` answers below.
                throw;
            }
            deferred = true;
        });
        if (!deferred)
            completion(result);
    }

    void downloadEnded(id download, bool completed) noexcept {
        const auto found = downloadDestinations.find(download);
        if (found == downloadDestinations.end())
            return;
        const auto pending = std::move(found->second);
        downloadDestinations.erase(found);
        if (completed && !pending.staging->finish(pending.target, pending.replace)) {
            completed = false;
            std::fprintf(stderr, "WebFront: the download could not be installed at %s\n", pending.target.string().c_str());
        }
        if (pending.reserved)
            reservations.release(pending.target);
        if (completed && options.downloadFinished)
            guard([&] {
                options.downloadFinished(pending.target.string());
            });
    }

    void navigationResponse(id response, void (^decision)(long)) noexcept {
        long policy = 1;  // WKNavigationResponsePolicyAllow
        guard([&] {
            if (!send<BOOL>(response, "isForMainFrame"))
                return;
            const auto http = send(response, "response");
            // Server redirects do not necessarily produce another navigation-action callback.
            if (!contentPolicy.allowsNavigation(utf8(send(send(http, "URL"), "absoluteString")))) {
                policy = 0;
                rejectNavigation("initial navigation response was denied by contentPolicy");
                return;
            }
            // A response the engine cannot display (an attachment, an archive) is saved, not shown.
            if (committed) {
                if (!send<BOOL>(response, "canShowMIMEType"))
                    policy = 2;  // WKNavigationResponsePolicyDownload
                return;
            }
            if (send<BOOL>(http, "respondsToSelector:", sel_registerName("statusCode"))) {
                const auto status = send<long>(http, "statusCode");
                if (!successfulHttpStatus(status)) {
                    policy = 0;
                    fail("initial navigation failed (HTTP " + std::to_string(status) + ")");
                } else {
                    receivedResponse = true;
                }
            }
        });
        decision(error ? 0 : policy);
    }
};

inline void sessionClosed(id self, SEL, id) {
    if (auto s = session(self))
        s->close();
}

inline unsigned long applicationShouldTerminate(id self, SEL selector, id app) {
    sessionClosed(self, selector, app);
    return 0;  // NSTerminateCancel: never terminate the embedding process.
}

inline BOOL terminateAfterLastWindow(id, SEL, id) {
    return false;
}

inline void startupTimedOut(id self, SEL, id) {
    if (auto s = session(self))
        s->fail("initial navigation timed out after " + std::to_string(s->options.startupTimeout.count()) + " milliseconds");
}

inline void navigationFinished(id self, SEL, id, id) {
    if (auto s = session(self)) {
        if (!s->receivedResponse) {
            // Some WebKit versions finish an internal error document without reporting
            // didFailNavigation. Such a document must not disable the startup deadline.
            s->fail("initial navigation failed without an HTTP response");
        } else {
            s->loaded = true;
            send<void>(s->timer, "invalidate");
        }
    }
}

inline void navigationCommitted(id self, SEL, id, id) {
    if (auto s = session(self))
        s->committed = true;
}

inline void navigationFailed(id self, id error) {
    if (auto s = session(self); s && !s->closed && !s->committed) {
        s->guard([&] {
            s->fail("initial navigation failed (" + utf8(send(error, "localizedDescription")) + ")");
        });
    }
}

inline void contentProcessTerminated(id self, SEL, id) {
    if (auto s = session(self))
        s->fail("web content process terminated");
}

inline void addApplicationMethods(Class cls) {
    addMethod(cls, "windowWillClose:", sessionClosed, "v@:@");
    addMethod(cls, "webViewDidClose:", sessionClosed, "v@:@");
    addMethod(cls, "webfrontQuit:", sessionClosed, "v@:@");
    addMethod(cls, "applicationShouldTerminate:", applicationShouldTerminate, "Q@:@");
    addMethod(cls, "applicationShouldTerminateAfterLastWindowClosed:", terminateAfterLastWindow, "B@:@");
    addMethod(cls, "webfrontTimeout:", startupTimedOut, "v@:@");
}

inline void addNavigationMethods(Class cls) {
    addMethod(
        cls,
        "webView:decidePolicyForNavigationAction:decisionHandler:",
        +[](id self, SEL, id, id action, void (^decision)(long)) {
            if (auto s = session(self))
                s->navigationAction(action, decision);
            else
                decision(0);
        },
        "v@:@@@?");
    addMethod(
        cls,
        "webView:decidePolicyForNavigationResponse:decisionHandler:",
        +[](id self, SEL, id, id response, void (^decision)(long)) {
            if (auto s = session(self))
                s->navigationResponse(response, decision);
            else
                decision(0);
        },
        "v@:@@@?");
    addMethod(
        cls,
        "webView:createWebViewWithConfiguration:forNavigationAction:windowFeatures:",
        +[](id, SEL, id, id, id, id) -> id {
            return nullptr;
        },
        "@@:@@@@");
    // File chooser (WKUIDelegate) and downloads (WKNavigationDelegate, WKDownloadDelegate), issue #451.
    addMethod(
        cls,
        "webView:runOpenPanelWithParameters:initiatedByFrame:completionHandler:",
        +[](id self, SEL, id, id parameters, id frame, void (^completion)(id)) {
            if (auto s = session(self))
                s->chooseFilesForPage(parameters, frame, completion);
            else
                completion(nullptr);
        },
        "v@:@@@@?");
    // JavaScript dialogs (WKUIDelegate), issue #452. Without these WebKit answers nothing and the page's call does not return.
    addMethod(
        cls,
        "webView:runJavaScriptAlertPanelWithMessage:initiatedByFrame:completionHandler:",
        +[](id self, SEL, id, id message, id frame, void (^completion)()) {
            auto handler = Block_copy(completion);
            auto done    = [handler](ScriptDialogAnswer) {
                handler();
                Block_release(handler);
            };
            if (auto s = session(self))
                s->runScriptDialog({ScriptDialog::Kind::alert, utf8(message), {}}, frame, done);
            else
                done({});
        },
        "v@:@@@@?");
    addMethod(
        cls,
        "webView:runJavaScriptConfirmPanelWithMessage:initiatedByFrame:completionHandler:",
        +[](id self, SEL, id, id message, id frame, void (^completion)(BOOL)) {
            auto handler = Block_copy(completion);
            auto done    = [handler](ScriptDialogAnswer reply) {
                handler(static_cast<BOOL>(reply.accepted));
                Block_release(handler);
            };
            if (auto s = session(self))
                s->runScriptDialog({ScriptDialog::Kind::confirm, utf8(message), {}}, frame, done);
            else
                done({});
        },
        "v@:@@@@?");
    addMethod(
        cls,
        "webView:runJavaScriptTextInputPanelWithPrompt:defaultText:initiatedByFrame:completionHandler:",
        +[](id self, SEL, id, id message, id defaultText, id frame, void (^completion)(id)) {
            auto handler = Block_copy(completion);
            auto done    = [handler](ScriptDialogAnswer reply) {
                Object value(reply.accepted ? text(reply.text) : nullptr);
                handler(value.get());  // nil is the prompt's null.
                Block_release(handler);
            };
            if (auto s = session(self))
                s->runScriptDialog({ScriptDialog::Kind::prompt, utf8(message), defaultText ? utf8(defaultText) : std::string{}}, frame, done);
            else
                done({});
        },
        "v@:@@@@@?");
    const auto becameDownload = +[](id self, SEL, id, id, id download) {
        send<void>(download, "setDelegate:", self);
    };
    addMethod(cls, "webView:navigationAction:didBecomeDownload:", becameDownload, "v@:@@@");
    addMethod(cls, "webView:navigationResponse:didBecomeDownload:", becameDownload, "v@:@@@");
    addMethod(
        cls,
        "download:decideDestinationUsingResponse:suggestedFilename:completionHandler:",
        +[](id self, SEL, id download, id, id suggested, void (^completion)(id)) {
            if (auto s = session(self))
                s->chooseDownloadDestination(download, utf8(suggested), completion);
            else
                completion(nullptr);
        },
        "v@:@@@@?");
    addMethod(
        cls,
        "downloadDidFinish:",
        +[](id self, SEL, id download) {
            if (auto s = session(self))
                s->downloadEnded(download, true);
        },
        "v@:@");
    addMethod(
        cls,
        "download:didFailWithError:resumeData:",
        +[](id self, SEL, id download, id, id) {
            if (auto s = session(self))
                s->downloadEnded(download, false);
        },
        "v@:@@@");
    addMethod(cls, "webView:didFinishNavigation:", navigationFinished, "v@:@@");
    addMethod(cls, "webView:didCommitNavigation:", navigationCommitted, "v@:@@");
    const auto failed = +[](id self, SEL, id, id, id error) {
        navigationFailed(self, error);
    };
    addMethod(cls, "webView:didFailProvisionalNavigation:withError:", failed, "v@:@@@");
    addMethod(cls, "webView:didFailNavigation:withError:", failed, "v@:@@@");
    addMethod(cls, "webViewWebContentProcessDidTerminate:", contentProcessTerminated, "v@:@");
}

inline Class delegateClass() {
    static Class cls = registerClass("WebFrontWKDelegate_v1", reinterpret_cast<Class>(type("NSObject")), [](Class result) {
        addApplicationMethods(result);
        addNavigationMethods(result);
    });
    return cls;
}

inline Class viewClass() {
    static Class cls = registerClass("WebFrontWKView_v1", reinterpret_cast<Class>(type("WKWebView")), [](Class result) {
        addMethod(
            result,
            "willOpenMenu:withEvent:",
            +[](id self, SEL selector, id menu, id event) {
                // WebKit may isa-swizzle the view for KVO. Start at WKWebView itself,
                // rather than the dynamic object's superclass (which could be our class).
                objc_super parent{self, reinterpret_cast<Class>(type("WKWebView"))};
                reinterpret_cast<void (*)(objc_super*, SEL, id, id)>(objc_msgSendSuper)(&parent, selector, menu, event);
                if (auto s = session(self); s && !s->options.contentPolicy.contextMenusEnabled)
                    send<void>(menu, "removeAllItems");
            },
            "v@:@@");
    });
    return cls;
}

inline void open(std::string_view address, std::string_view port, std::string_view file, const WKWebViewOptions& options) {
    initialize();
    if (opening)
        throw std::runtime_error("WKWebView: nested open() calls are unsupported");
    struct Opening {
        Opening() {
            opening = true;
        }
        ~Opening() {
            opening = false;
        }
    } openingScope;
    Object  pool(send(type("NSAutoreleasePool"), "new"));
    Session window(applicationUrl(address, port, file), options);
    window.create();
    window.start();
    window.run();
}

}  // namespace webfront::frontend::detail::wkwebview
#endif

namespace webfront::frontend {

/** @brief Optional macOS embedded window. Main thread only; see docs/WKWEBVIEW.md. */
struct WKWebViewFrontend {
    static constexpr Action action{Action::closeServerAfterOpen};

    static void initialize() {
#if defined(WEBFRONT_EMBED_WKWEBVIEW) && defined(__APPLE__)
        detail::wkwebview::initialize();
#else
        throw std::runtime_error("WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON");
#endif
    }

    static void open(std::string_view port, std::string_view file) {
        open("127.0.0.1", port, file);
    }
    template <typename Options>
        requires std::same_as<std::remove_cvref_t<Options>, WKWebViewOptions>
    static void open(std::string_view address, std::string_view port, std::string_view file, Options&& options) {
#if defined(WEBFRONT_EMBED_WKWEBVIEW) && defined(__APPLE__)
        detail::wkwebview::open(address, port, file, options);
#else
        (void)address;
        (void)port;
        (void)file;
        (void)options;
        initialize();
#endif
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy = {}) {
        open(address, port, file, WKWebViewOptions{.contentPolicy = policy});
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
        open(address, port, file, WKWebViewOptions{.contentPolicy = policy, .window = window});
    }
};

}  // namespace webfront::frontend
