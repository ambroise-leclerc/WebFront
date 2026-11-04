#pragma once

#include "../ContentPolicy.hpp"
#include "../tooling/Logger.hpp"
#include "Action.hpp"
#include "Capture.hpp"
#include "DefaultBrowser.hpp"
#include "Downloads.hpp"
#include "ScriptDialogs.hpp"
#include "Url.hpp"
#include "Window.hpp"
#include "../ViewAssociation.hpp"
#include "WebKitGTKPolicy.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(WEBFRONT_EMBED_WEBKITGTK) && defined(__linux__)
    #include <cstdint>
    #include <cstdlib>
    #include <exception>

    #include <dlfcn.h>
    #include <sys/syscall.h>
    #include <unistd.h>

namespace webfront::frontend::detail::webkitgtk {

// Only the stable C ABI crosses this boundary. In particular no GTK, GLib or X11 header
// is included in a consumer translation unit. Keep the module loaded for the process:
// GTK registers process-global GTypes whose callbacks must never point into an unloaded DSO.
class Api {
    void* module = dlopen("libwebkitgtk-6.0.so.4", RTLD_NOW | RTLD_LOCAL);

    template <typename Function>
    Function symbol(const char* name) {
        if (!module)
            throw std::runtime_error("WebKitGTK: cannot load libwebkitgtk-6.0.so.4; install the WebKitGTK 6.0 runtime");
        const auto result = reinterpret_cast<Function>(dlsym(module, name));
        if (!result)
            throw std::runtime_error(std::string("WebKitGTK: missing runtime symbol ") + name);
        return result;
    }

    // A symbol that only some runtimes provide: null instead of an exception, so the rest of the adapter keeps working.
    template <typename Function>
    Function optionalSymbol(const char* name) {
        return module ? reinterpret_cast<Function>(dlsym(module, name)) : nullptr;
    }

public:
    // Opaque C object pointers deliberately stay private to the adapter's ABI boundary.
    using Object   = void*;
    using Callback = void (*)();
    using Destroy  = void (*)(void*, void*);
    struct Error {
        unsigned domain;
        int      code;
        char*    message;
    };

    int (*init)()                                                                 = symbol<decltype(init)>("gtk_init_check");
    Object (*windowNew)()                                                         = symbol<decltype(windowNew)>("gtk_window_new");
    void (*windowTitle)(Object, const char*)                                      = symbol<decltype(windowTitle)>("gtk_window_set_title");
    const char* (*windowGetTitle)(Object)                                         = symbol<decltype(windowGetTitle)>("gtk_window_get_title");
    void (*windowSize)(Object, int, int)                                          = symbol<decltype(windowSize)>("gtk_window_set_default_size");
    void (*windowChild)(Object, Object)                                           = symbol<decltype(windowChild)>("gtk_window_set_child");
    void (*windowPresent)(Object)                                                 = symbol<decltype(windowPresent)>("gtk_window_present");
    void (*windowDestroy)(Object)                                                 = symbol<decltype(windowDestroy)>("gtk_window_destroy");
    Object (*loopNew)(Object, int)                                                = symbol<decltype(loopNew)>("g_main_loop_new");
    void (*loopRun)(Object)                                                       = symbol<decltype(loopRun)>("g_main_loop_run");
    int (*loopIsRunning)(Object)                                                  = symbol<decltype(loopIsRunning)>("g_main_loop_is_running");
    void (*loopQuit)(Object)                                                      = symbol<decltype(loopQuit)>("g_main_loop_quit");
    void (*loopUnref)(Object)                                                     = symbol<decltype(loopUnref)>("g_main_loop_unref");
    unsigned long (*connect)(Object, const char*, Callback, Object, Destroy, int) = symbol<decltype(connect)>("g_signal_connect_data");
    void (*disconnect)(Object, unsigned long)                                     = symbol<decltype(disconnect)>("g_signal_handler_disconnect");
    void (*unref)(Object)                                                         = symbol<decltype(unref)>("g_object_unref");
    Object (*refSink)(Object)                                                     = symbol<decltype(refSink)>("g_object_ref_sink");
    Object (*objectNew)(std::uintptr_t, const char*, ...)                         = symbol<decltype(objectNew)>("g_object_new");
    void (*objectSet)(Object, const char*, ...)                                   = symbol<decltype(objectSet)>("g_object_set");
    std::uintptr_t (*viewType)()                                                  = symbol<decltype(viewType)>("webkit_web_view_get_type");
    Object (*networkNew)(const char*, const char*)                                = symbol<decltype(networkNew)>("webkit_network_session_new");
    Object (*settings)(Object)                                                    = symbol<decltype(settings)>("webkit_web_view_get_settings");
    void (*load)(Object, const char*)                                             = symbol<decltype(load)>("webkit_web_view_load_uri");
    const char* (*viewUri)(Object)                                                = symbol<decltype(viewUri)>("webkit_web_view_get_uri");
    const char* (*viewTitle)(Object)                                              = symbol<decltype(viewTitle)>("webkit_web_view_get_title");
    void (*windowClose)(Object)                                                   = symbol<decltype(windowClose)>("gtk_window_close");
    void (*stop)(Object)                                                          = symbol<decltype(stop)>("webkit_web_view_stop_loading");
    Object (*navigationAction)(Object)                      = symbol<decltype(navigationAction)>("webkit_navigation_policy_decision_get_navigation_action");
    Object (*request)(Object)                               = symbol<decltype(request)>("webkit_navigation_action_get_request");
    const char* (*uri)(Object)                              = symbol<decltype(uri)>("webkit_uri_request_get_uri");
    int (*userGesture)(Object)                              = symbol<decltype(userGesture)>("webkit_navigation_action_is_user_gesture");
    void (*ignore)(Object)                                  = symbol<decltype(ignore)>("webkit_policy_decision_ignore");
    int (*mainResource)(Object)                             = symbol<decltype(mainResource)>("webkit_response_policy_decision_is_main_frame_main_resource");
    const char* (*responseUri)(Object)                      = symbol<decltype(responseUri)>("webkit_uri_response_get_uri");
    unsigned (*timeoutAdd)(unsigned, int (*)(void*), void*) = symbol<decltype(timeoutAdd)>("g_timeout_add");
    int (*sourceRemove)(unsigned)                           = symbol<decltype(sourceRemove)>("g_source_remove");
    Object (*response)(Object)                              = symbol<decltype(response)>("webkit_response_policy_decision_get_response");
    unsigned (*status)(Object)                              = symbol<decltype(status)>("webkit_uri_response_get_status_code");
    void (*evaluate)(Object, const char*, long, const char*, const char*, Object, Callback, Object) =
        symbol<decltype(evaluate)>("webkit_web_view_evaluate_javascript");
    unsigned (*idleAddFull)(int, int (*)(void*), void*, void (*)(void*)) = symbol<decltype(idleAddFull)>("g_idle_add_full");
    // Capture needs GTK 4.6 (gdk_texture_save_to_png_bytes): an older runtime still opens windows, but cannot capture.
    void (*snapshot)(Object, int, int, Object, Callback, void*) = optionalSymbol<decltype(snapshot)>("webkit_web_view_get_snapshot");
    Object (*snapshotFinish)(Object, Object, Error**)           = optionalSymbol<decltype(snapshotFinish)>("webkit_web_view_get_snapshot_finish");
    Object (*texturePng)(Object)                                = optionalSymbol<decltype(texturePng)>("gdk_texture_save_to_png_bytes");
    const void* (*bytesData)(Object, std::size_t*)              = symbol<decltype(bytesData)>("g_bytes_get_data");
    void (*bytesUnref)(Object)                                  = symbol<decltype(bytesUnref)>("g_bytes_unref");
    void (*errorFree)(Error*)                                   = symbol<decltype(errorFree)>("g_error_free");
    // GLib resolves the folder from user-dirs.dirs (a user may have moved it), not only from the environment.
    const char* (*homeDirectory)() = symbol<decltype(homeDirectory)>("g_get_home_dir");
    const char* (*userDirectory)(int) = symbol<decltype(userDirectory)>("g_get_user_special_dir");  // G_USER_DIRECTORY_DOWNLOAD = 2
    void (*downloadDestination)(Object, const char*)            = symbol<decltype(downloadDestination)>("webkit_download_set_destination");
    void (*downloadCancel)(Object)                              = symbol<decltype(downloadCancel)>("webkit_download_cancel");
    const char* (*downloadGetDestination)(Object)               = symbol<decltype(downloadGetDestination)>("webkit_download_get_destination");
    void (*chooserSelect)(Object, const char* const*)           = symbol<decltype(chooserSelect)>("webkit_file_chooser_request_select_files");
    void (*chooserCancel)(Object)                               = symbol<decltype(chooserCancel)>("webkit_file_chooser_request_cancel");
    // WEBKIT_SCRIPT_DIALOG_ALERT = 0, CONFIRM = 1, PROMPT = 2, BEFORE_UNLOAD_CONFIRM = 3.
    int (*dialogType)(Object)                                   = symbol<decltype(dialogType)>("webkit_script_dialog_get_dialog_type");
    const char* (*dialogMessage)(Object)                        = symbol<decltype(dialogMessage)>("webkit_script_dialog_get_message");
    void (*dialogConfirm)(Object, int)                          = symbol<decltype(dialogConfirm)>("webkit_script_dialog_confirm_set_confirmed");
    const char* (*dialogDefault)(Object)                        = symbol<decltype(dialogDefault)>("webkit_script_dialog_prompt_get_default_text");
    void (*dialogPromptText)(Object, const char*)               = symbol<decltype(dialogPromptText)>("webkit_script_dialog_prompt_set_text");
    // User scripts: WebKitUserContentInjectedFrames TOP_FRAME = 1 and WebKitUserScriptInjectionTime START = 0.
    Object (*contentManager)(Object)                            = symbol<decltype(contentManager)>("webkit_web_view_get_user_content_manager");
    Object (*scriptNew)(const char*, int, int, const char* const*, const char* const*) = symbol<decltype(scriptNew)>("webkit_user_script_new");
    void (*scriptAdd)(Object, Object)                           = symbol<decltype(scriptAdd)>("webkit_user_content_manager_add_script");
    void (*scriptUnref)(Object)                                 = symbol<decltype(scriptUnref)>("webkit_user_script_unref");
};

inline Api& api() {
    static Api instance;
    return instance;
}

inline void requireMainThread() {
    if (syscall(SYS_gettid) != getpid())
        throw std::runtime_error("WebKitGTK: construct the application and call show()/open() on the process main thread");
}

inline void initialize() {
    requireMainThread();
    if (!api().init())
        throw std::runtime_error("WebKitGTK: cannot open a display; set DISPLAY or WAYLAND_DISPLAY for a working desktop, or run under xvfb-run -a");
}

inline std::filesystem::path xdgRoot(const char* variable, const char* fallback) {
    const auto* value = std::getenv(variable);
    if (value && std::filesystem::path(value).is_absolute())
        return value;
    const auto* home = std::getenv("HOME");
    if (home && std::filesystem::path(home).is_absolute())
        return std::filesystem::path(home) / fallback;
    throw std::runtime_error(std::string("WebKitGTK: set ") + variable + " or HOME to an absolute writable directory");
}

inline std::string executableProfileName() {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    // Stable across launches and independent of std::hash's implementation.
    std::uint64_t hash = 14695981039346656037ULL;
    for (char byte : executable.string())
        hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    return executable.filename().string() + "-" + std::to_string(hash);
}

inline std::filesystem::path profileDirectory(const char* variable, const char* fallback) {
    const auto directory = xdgRoot(variable, fallback) / "webfront" / executableProfileName();
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    if (access(directory.c_str(), W_OK | X_OK) != 0)
        throw std::runtime_error("WebKitGTK: profile directory is not writable: " + directory.string());
    return directory;
}

/// The view as seen by capture requests: read and cleared only on the GTK main thread, so a request that
/// outlives the window finds it null.
struct CaptureJob;
struct CaptureTarget {
    Api::Object                             view = nullptr;
    PendingCaptures<CaptureJob> pending;  // Requests nobody has answered yet, queued or in flight.
};

struct CaptureJob {
    std::shared_ptr<CaptureTarget> target;
    std::promise<CaptureResult>    done;
    std::atomic<bool>              abandoned = false;  // The requester gave up: the main thread must not start it.
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


// Runs on the GTK main thread; owns the job until the snapshot completes.
inline void finishCapture(Api::Object view, Api::Object result, void* owner) noexcept {
    std::unique_ptr<std::shared_ptr<CaptureJob>> job{static_cast<std::shared_ptr<CaptureJob>*>(owner)};
    auto&                                        a = api();
    (*job)->target->pending.untrack(*job);
    try {
        Api::Error* error   = nullptr;
        const auto  texture = a.snapshotFinish(view, result, &error);
        if (!texture) {
            std::string message = error && error->message ? error->message : "the engine returned no image";
            if (error)
                a.errorFree(error);
            answer(**job, std::unexpected("snapshot failed: " + message));
            return;
        }
        const auto bytes = a.texturePng(texture);
        a.unref(texture);
        if (!bytes) {
            answer(**job, std::unexpected("cannot encode the snapshot as PNG"));
            return;
        }
        std::size_t               size  = 0;
        const auto*               begin = static_cast<const std::uint8_t*>(a.bytesData(bytes, &size));
        std::vector<std::uint8_t> png(begin, begin + size);
        a.bytesUnref(bytes);
        answer(**job, std::move(png));
    } catch (...) {
        try {
            (*job)->done.set_exception(std::current_exception());
        } catch (...) { }
    }
}

// The idle source owns `data` (see releaseCapture): it is freed whether the source ran or was removed unserved.
inline int startCapture(void* data) noexcept {
    const auto& job = *static_cast<std::shared_ptr<CaptureJob>*>(data);
    auto&       a   = api();
    if (job->abandoned)
        return 0;
    if (!job->target->view) {
        answer(*job, std::unexpected("the window is closed"));
        return 0;
    }
    const auto view = job->target->view;
    // WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE. finishCapture owns its own reference to the job.
    a.snapshot(view, 0, 0, nullptr, reinterpret_cast<Api::Callback>(finishCapture), new std::shared_ptr<CaptureJob>(job));
    return 0;
}

inline void releaseCapture(void* data) noexcept {
    delete static_cast<std::shared_ptr<CaptureJob>*>(data);
}

inline CaptureProvider captureProvider(std::shared_ptr<CaptureTarget> target) {
    return [target = std::move(target)](std::chrono::milliseconds timeout) -> CaptureResult {
        const auto& a = api();
        if (!a.snapshot || !a.snapshotFinish || !a.texturePng)
            return std::unexpected("this WebKitGTK/GTK runtime cannot capture (GTK 4.6 is required)");
        auto job    = std::make_shared<CaptureJob>();
        job->target = target;
        auto future = job->done.get_future();
        if (!target->pending.track(job))
            return std::unexpected("the window is closed");
        constexpr int idlePriority = 200;  // G_PRIORITY_DEFAULT_IDLE
        a.idleAddFull(idlePriority, startCapture, new std::shared_ptr<CaptureJob>(job), releaseCapture);
        const auto answered = future.wait_for(timeout) == std::future_status::ready;
        target->pending.untrack(job);  // Whatever the outcome, a finished request is not kept until the window closes.
        if (!answered) {
            // The source is never touched from this thread: GLib hands out no counted reference to it, so one that the
            // main thread is serving could be freed under us. The flag makes it a no-op when it runs; a main loop that
            // never runs keeps only this small job until the process ends.
            job->abandoned = true;
            return std::unexpected("the window did not answer within the capture timeout");
        }
        return future.get();
    };
}

struct Session : FailureState {
    Api&                                 a       = api();
    Api::Object                          loop    = a.loopNew(nullptr, 0);
    Api::Object                          window  = nullptr;
    Api::Object                          view    = nullptr;
    Api::Object                          network = nullptr;
    webfront::detail::BoundContentPolicy policy;
    WindowOptions                        windowOptions;
    std::function<void()>                navigationCancelled;
    /// Per-launch proof of this view (ADR-020), given at the start of the top-level documents of the application's origin.
    ViewLaunch launch;
    /// Download folder; empty selects the user's XDG download folder, else the home folder.
    std::filesystem::path downloadDirectory;
    /// Test injection: answers <input type="file"> without a dialog (an empty list cancels). Unset shows the system dialog.
    std::function<std::vector<std::string>()> chooseFiles;
    /// Test hook: called when the system file dialog is about to open.
    std::function<void()> chooserOpened;
    /// Test injection: called with the destination of each finished download.
    std::function<void(const std::string&)> downloadFinished;
    /// Test injection: answers alert, confirm, prompt and beforeunload without a dialog. Unset shows WebKitGTK's own dialog.
    ScriptDialogHandler answerScriptDialog;
    /// Test hook: called when WebKitGTK's own script dialog is about to open.
    std::function<void()> scriptDialogOpened;
    struct Download {
        Api::Object                object;
        std::vector<unsigned long> handlers;
        std::filesystem::path      reserved;  // The destination given to this download, until it ends.
    };
    DownloadReservations reservations;
    std::vector<Download> downloads;
    unsigned long         downloadStartedHandler = 0;
    bool                                 closed            = false;
    bool                                 initialNavigation = true;
    unsigned                             navigationTimer   = 0;
    unsigned long                        windowHandler     = 0;
    std::vector<unsigned long>           viewHandlers;
    std::shared_ptr<CaptureTarget>       captureTarget = std::make_shared<CaptureTarget>();
    CaptureRegistration                  captureRegistration;

    explicit Session(webfront::detail::BoundContentPolicy rules, WindowOptions settings = {})
        : policy(std::move(rules)), windowOptions(resolveWindow(std::move(settings))) {}
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    ~Session() {
        stopNavigationTimer();
        releaseDownloads();
        releaseView();
        destroyWindow();
        release(network);
        a.loopUnref(loop);
    }

    void release(Api::Object& object) {
        if (object)
            a.unref(std::exchange(object, nullptr));
    }
    void stopNavigationTimer() {
        if (navigationTimer)
            a.sourceRemove(std::exchange(navigationTimer, 0));
    }
    void releaseView() {
        captureRegistration = {};
        abandonCaptures(*captureTarget);
        if (!view)
            return;
        for (auto handler : viewHandlers)
            a.disconnect(view, handler);
        a.stop(view);
        release(view);
    }
    void releaseDownloads() {
        if (downloadStartedHandler && network)
            a.disconnect(network, std::exchange(downloadStartedHandler, 0));
        for (auto& download : downloads) {
            for (auto handler : download.handlers)
                a.disconnect(download.object, handler);
            a.downloadCancel(download.object);
            a.unref(download.object);
        }
        downloads.clear();
    }
    void destroyWindow() {
        if (!window)
            return;
        if (windowHandler)
            a.disconnect(window, windowHandler);
        a.windowDestroy(std::exchange(window, nullptr));
    }

    void close() noexcept {
        closed = true;
        a.loopQuit(loop);
    }
    void failAndClose(std::string_view message) noexcept {
        FailureState::fail(message);
        close();
    }
    template <typename Function>
    void guard(Function operation) noexcept {
        try {
            operation();
        } catch (...) {
            rememberFailure();
            close();
        }
    }
    void evaluate(const char* script) {
        a.evaluate(view, script, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    template <typename Function>
    void on(const char* signal, Function callback, int flags = 0) {
        viewHandlers.push_back(a.connect(view, signal, reinterpret_cast<Api::Callback>(callback), this, nullptr, flags));
    }

    void create() {
        const auto data  = profileDirectory("XDG_DATA_HOME", ".local/share");
        const auto cache = profileDirectory("XDG_CACHE_HOME", ".cache");
        network          = a.networkNew(data.c_str(), cache.c_str());
        view             = a.refSink(a.objectNew(a.viewType(), "network-session", network, nullptr));
        window           = a.windowNew();
        a.windowTitle(window, windowOptions.title.c_str());
        a.windowSize(window, windowOptions.width, windowOptions.height);
        a.windowChild(window, view);
        configureSettings();
        installViewAssociation();
        registerHandlers();
        downloadStartedHandler = a.connect(network, "download-started", reinterpret_cast<Api::Callback>(onDownloadStarted), this, nullptr, 0);
        captureTarget->view = view;
        captureTarget->pending.open();
        captureRegistration = CaptureRegistration{captureProvider(captureTarget)};
    }

    void installViewAssociation() {
        if (launch.secret.empty())
            return;
        const auto source  = webfront::detail::viewSecretScript(launch.secret, launch.origin);
        const auto pattern = launch.origin + "/*";
        const char* const allowList[] = {pattern.c_str(), nullptr};
        const auto script = a.scriptNew(source.c_str(), 1, 0, allowList, nullptr);
        a.scriptAdd(a.contentManager(view), script);
        a.scriptUnref(script);
    }

    void configureSettings() {
        a.objectSet(a.settings(view),
                    "javascript-can-open-windows-automatically",
                    1,
                    "allow-modal-dialogs",
                    0,
                    "enable-developer-extras",
                    policy.options().devToolsEnabled ? 1 : 0,
                    nullptr);
    }
    void registerHandlers() {
        viewHandlers.reserve(6);
        windowHandler = a.connect(window, "close-request", reinterpret_cast<Api::Callback>(onCloseRequest), this, nullptr, 0);
        on("close", onViewClose);
        on("context-menu", onContextMenu);
        on("load-changed", onLoadChanged);
        // G_CONNECT_SWAPPED puts Session first; the unused trailing WebView argument is omitted.
        on("load-failed", onLoadFailed, 2);
        on("web-process-terminated", onProcessTerminated);
        on("decide-policy", onDecidePolicy);
        on("run-file-chooser", onRunFileChooser);
        on("script-dialog", onScriptDialog);
    }

    std::filesystem::path destinationFolder() const {
        if (!downloadDirectory.empty())
            return downloadDirectory;
        const auto* home = std::getenv("HOME");
        if (!home || !*home)
            home = a.homeDirectory();  // GLib reads the passwd database when HOME is unset.
        if (const auto* folder = a.userDirectory(2); folder && *folder) {
            std::error_code error;
            // GLib reports the home folder itself when no download folder is configured: never write there.
            if (!(home && *home && std::filesystem::equivalent(folder, home, error)))
                return folder;
        }
        if (home && *home)
            return std::filesystem::path(home) / "Downloads";  // Created on demand.
        return std::filesystem::temp_directory_path();
    }
    std::filesystem::path reserveDestination(std::string_view suggested) {
        const auto      folder = destinationFolder();
        std::error_code error;
        std::filesystem::create_directories(folder, error);  // WebKitGTK writes into an existing folder only.
        if (error)
            throw std::filesystem::filesystem_error("WebKitGTK: cannot create the download folder", folder, error);
        return reservations.reserve(folder, suggested);
    }
    void trackDownload(Api::Object download) {
        Download entry{download, {}, {}};
        a.refSink(download);
        entry.handlers.push_back(a.connect(download, "decide-destination", reinterpret_cast<Api::Callback>(onDecideDestination), this, nullptr, 0));
        entry.handlers.push_back(a.connect(download, "finished", reinterpret_cast<Api::Callback>(onDownloadFinished), this, nullptr, 0));
        entry.handlers.push_back(a.connect(download, "failed", reinterpret_cast<Api::Callback>(onDownloadFailed), this, nullptr, 0));
        downloads.push_back(std::move(entry));
    }
    void forgetDownload(Api::Object download) {
        const auto found = std::ranges::find(downloads, download, &Download::object);
        if (found == downloads.end())
            return;
        for (auto handler : found->handlers)
            a.disconnect(download, handler);
        if (!found->reserved.empty())
            reservations.release(found->reserved);
        a.unref(found->object);
        downloads.erase(found);
    }

    bool initialDocumentFailed() const {
        if (!initialNavigation)
            return false;
        const auto* url = a.viewUri(view);
        if (!url)
            return true;
        return !policy.allowsNavigation(url);
    }
    void navigationCommitted() {
        if (initialDocumentFailed()) {
            failAndClose("initial navigation failed: engine committed an error document; check the local URL");
            return;
        }
        initialNavigation = false;
        stopNavigationTimer();
    }
    void loadFailed(Api::Error* error) {
        if (initialNavigation && !closed)
            failAndClose(initialNavigationError(error ? error->message : nullptr));
    }
    PolicyDecision responseDecision(Api::Object decision) const {
        if (!a.mainResource(decision))
            return PolicyDecision::allow;
        const auto resource = a.response(decision);
        if (!resource)
            return PolicyDecision::cancel;
        return decidePolicy(policy, {PolicyEvent::response, a.responseUri(resource), true, a.status(resource), initialNavigation});
    }
    void forwardPopup(Api::Object action, const char* url) const {
        try {
            policy.handlePopup(url ? url : "", a.userGesture(action) != 0, browser::launch);
        } catch (const std::exception& error) {
            log::error("WebKitGTK popup launch failed: {}", error.what());
        } catch (...) {
            log::error("WebKitGTK popup launch failed with a non-standard exception");
        }
    }
    int applyPolicyDecision(Api::Object decision, PolicyDecision result) {
        if (result == PolicyDecision::allow)
            return 0;
        a.ignore(decision);
        if (result == PolicyDecision::failInitialNavigation)
            failAndClose("initial navigation returned an HTTP error; check the page path");
        else if (navigationCancelled)
            navigationCancelled();
        return 1;
    }
    int policyDecision(Api::Object decision, PolicyEvent event) {
        if (event == PolicyEvent::response)
            return applyPolicyDecision(decision, responseDecision(decision));
        const auto  action = a.navigationAction(decision);
        const auto* url    = a.uri(a.request(action));
        const auto  result = decidePolicy(policy, {event, url});
        if (result != PolicyDecision::popup)
            return applyPolicyDecision(decision, result);
        a.ignore(decision);
        forwardPopup(action, url);
        return 1;
    }

    static int onCloseRequest(Api::Object, Session* self) noexcept {
        self->close();
        return 1;  // Destruction is centralized in the session destructor.
    }
    static void onViewClose(Api::Object, Session* self) noexcept {
        self->close();
    }
    static int onContextMenu(Api::Object, Api::Object, Api::Object, Session* self) noexcept {
        return self->policy.options().contextMenusEnabled ? 0 : 1;
    }
    static void onLoadChanged(Api::Object, int event, Session* self) noexcept {
        if (event == 2)  // WEBKIT_LOAD_COMMITTED: slow subresources are not a startup failure.
            self->guard([self] {
                self->navigationCommitted();
            });
    }
    static int onLoadFailed(Session* self, int, const char*, Api::Error* error) noexcept {
        self->guard([self, error] {
            self->loadFailed(error);
        });
        return 1;
    }
    static void onProcessTerminated(Api::Object, int, Session* self) noexcept {
        if (!self->closed)
            self->failAndClose("web process terminated; check the WebKitGTK runtime and sandbox configuration");
    }
    static int onDecidePolicy(Api::Object, Api::Object decision, int type, Session* self) noexcept {
        try {
            return self->policyDecision(decision, static_cast<PolicyEvent>(type));
        } catch (...) {
            self->rememberFailure();
            self->close();
            return 1;
        }
    }
    static void onDownloadStarted(Api::Object, Api::Object download, Session* self) noexcept {
        self->guard([self, download] {
            self->trackDownload(download);
        });
    }
    static int onDecideDestination(Api::Object download, const char* suggested, Session* self) noexcept {
        try {
            const auto destination = self->reserveDestination(suggested ? suggested : "");
            if (const auto entry = std::ranges::find(self->downloads, download, &Download::object); entry != self->downloads.end())
                entry->reserved = destination;
            else
                self->reservations.release(destination);
            api().downloadDestination(download, destination.c_str());
        } catch (...) {
            api().downloadCancel(download);
        }
        return 1;
    }
    static void onDownloadFinished(Api::Object download, Session* self) noexcept {
        std::string destination;
        if (const auto* path = api().downloadGetDestination(download))
            destination = path;
        self->guard([self, download, destination] {
            self->forgetDownload(download);
            if (self->downloadFinished)
                self->downloadFinished(destination);
        });
    }
    static void onDownloadFailed(Api::Object download, Api::Error*, Session* self) noexcept {
        self->guard([self, download] {
            self->forgetDownload(download);
        });
    }
    static int onRunFileChooser(Api::Object, Api::Object request, Session* self) noexcept {
        if (!self->chooseFiles) {
            if (self->chooserOpened)
                self->chooserOpened();
            return 0;  // The system dialog, attached to the window by WebKitGTK.
        }
        try {
            const auto              files = self->chooseFiles();
            std::vector<const char*> pointers;
            for (const auto& file : files)
                pointers.push_back(file.c_str());
            pointers.push_back(nullptr);
            if (files.empty())
                api().chooserCancel(request);
            else
                api().chooserSelect(request, pointers.data());
        } catch (...) {
            api().chooserCancel(request);
        }
        return 1;
    }
    static int onScriptDialog(Api::Object, Api::Object dialog, Session* self) noexcept {
        if (!self->answerScriptDialog) {
            if (self->scriptDialogOpened)
                self->scriptDialogOpened();
            return 0;  // WebKitGTK's dialog, attached to the window; closing the window dismisses it.
        }
        auto&        a = api();
        ScriptDialog description;
        switch (a.dialogType(dialog)) {
            case 1: description.kind = ScriptDialog::Kind::confirm; break;
            case 2: description.kind = ScriptDialog::Kind::prompt; break;
            case 3: description.kind = ScriptDialog::Kind::beforeUnload; break;
            default: description.kind = ScriptDialog::Kind::alert; break;
        }
        if (const auto* message = a.dialogMessage(dialog))
            description.message = message;
        if (description.kind == ScriptDialog::Kind::prompt)
            if (const auto* initial = a.dialogDefault(dialog))
                description.defaultText = initial;
        ScriptDialogAnswer reply;
        try {
            reply = self->answerScriptDialog(description);
        } catch (...) {
            reply = {};  // A failing injection dismisses the dialog; the page must not wait for it.
        }
        if (description.kind == ScriptDialog::Kind::prompt)
            a.dialogPromptText(dialog, reply.accepted ? reply.text.c_str() : nullptr);  // A null text is the prompt's null.
        else if (description.kind != ScriptDialog::Kind::alert) {
            a.dialogConfirm(dialog, reply.accepted ? 1 : 0);
        }
        return 1;
    }
    static int onNavigationTimeout(void* data) noexcept {
        auto* self            = static_cast<Session*>(data);
        self->navigationTimer = 0;
        self->failAndClose("initial navigation timed out after 30 seconds; check the local server");
        return 0;
    }

    void run(const std::string& url) {
        rethrowIfFailed();
        if (closed)
            return;
        navigationTimer = a.timeoutAdd(30'000, onNavigationTimeout, this);
        a.windowPresent(window);
        a.load(view, url.c_str());
        if (!closed)
            a.loopRun(loop);
        rethrowIfFailed();
    }
};

struct OpenOptions {
    ContentPolicy                 contentPolicy{};
    WindowOptions                 window{};
    std::function<void(Session&)> configure{};
    ViewLaunch                    launch{};
};

inline void open(std::string_view address, std::string_view port, std::string_view file, const OpenOptions& options = {}) {
    initialize();
    Session session(webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), options.contentPolicy}, options.window);
    session.launch = options.launch;
    session.create();
    if (options.configure)
        options.configure(session);
    session.run(applicationUrl(address, port, file));
}

}  // namespace webfront::frontend::detail::webkitgtk
#endif

namespace webfront::frontend {

/** @brief Optional Linux WebKitGTK 6.0 window. Construct and open on the process main thread. */
struct WebKitGTKFrontend {
    static constexpr Action action{Action::closeServerAfterOpen};
    static void             initialize() {
#if defined(WEBFRONT_EMBED_WEBKITGTK) && defined(__linux__)
        detail::webkitgtk::initialize();
#else
        throw std::runtime_error("WebKitGTKFrontend is unavailable. Configure on Linux with -DWEBFRONT_EMBED_WEBKITGTK=ON");
#endif
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page);
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy = {}) {
        open(address, port, page, policy, WindowOptions{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy, const WindowOptions& window) {
#if defined(WEBFRONT_EMBED_WEBKITGTK) && defined(__linux__)
        detail::webkitgtk::open(address, port, page, {.contentPolicy = policy, .window = window});
#else
        (void)window;
        (void)address;
        (void)port;
        (void)page;
        (void)policy;
        initialize();
#endif
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy, const WindowOptions& window,
                     const ViewLaunch& launch) {
#if defined(WEBFRONT_EMBED_WEBKITGTK) && defined(__linux__)
        detail::webkitgtk::open(address, port, page, {.contentPolicy = policy, .window = window, .launch = launch});
#else
        (void)launch;
        open(address, port, page, policy, window);
#endif
    }
};

}  // namespace webfront::frontend
