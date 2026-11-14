/// @brief A WebFront application on a thread of its own, for a host that keeps its calling thread (#567).
///
/// `show()` runs an embedded window's loop on the thread that calls it. A host application that already has its own loop
/// on its main thread gives WebFront another one with `UIThread<App>`: that thread constructs the application (the
/// frontend's initialization has the same thread affinity as its window), runs the host's `setup`, then `show()`.
/// See docs/HOST_INTEGRATION.md for the three integration paths and the threads involved.
#pragma once

#include "WebFront.hpp"

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace webfront {

/// @brief UIThread::stop() gave up: the window did not close in the time allowed, and its thread still runs show().
class UIThreadStalled : public std::runtime_error {
public:
    UIThreadStalled() : std::runtime_error("webfront::UIThread::stop(): the interface did not close when asked; its thread is still running") { }
};

/**
 * @brief Owns the thread on which an application is constructed, shown and closed.
 *
 * - **Start.** The constructor returns once the application exists and `setup(app)` has run on the interface thread:
 *   the server is listening (`app().port()`) and the window is about to open. An exception thrown by the application's
 *   construction (the frontend's initialization included) or by `setup` is rethrown by the constructor, after the thread
 *   was joined. A failure of the opening itself happens later: `running()` becomes false, `failure()` holds it and
 *   `stop()` rethrows it.
 * - **Exchanges.** `app()` is the application, for the calls documented as callable from any thread (broadcast,
 *   forEachUI, spawn, subscriptions, requestClose). Callbacks still run on the application's server thread, which is
 *   neither the host's thread nor the interface thread.
 * - **End.** The user closing the window ends `show()`: `running()` turns false and the application stays alive, so that
 *   references kept by the host remain valid, until `stop()` or destruction. `stop()` asks the window to close, joins the
 *   thread and destroys the application; no thread outlives the object.
 *
 * `App` is a `BasicWF` whose frontend runs on a secondary thread and can be asked to close (`requestClose()`): WebView2,
 * and the system browser's loop mode outside macOS. A frontend that belongs to the main thread (WKWebView, WebKitGTK,
 * CEF) is refused at compile time: call `show()` on the main thread and move the host's work instead. The plain system
 * browser needs no thread at all: use `App::start()`.
 *
 * One object is used by one host thread: app(), requestClose() and stop() are not synchronized with one another, and the
 * mutex inside only orders what the interface thread writes (the end of show() and its failure) with what the host
 * reads. Do not call stop() or destroy it from a WebFront callback or from `setup`.
 *
 * stop() is bounded and reports a window that does not close; the destructor has to wait for its thread and is not.
 */
template <typename App>
class UIThread {
    using Frontend = typename App::FrontendProvider;

    static_assert(!frontend::requiresMainThread<Frontend>,
                  "webfront::UIThread: this frontend's window belongs to the process main thread. Construct the application and "
                  "call show() there, run the host's own work on other threads and end the session with requestClose() "
                  "(docs/HOST_INTEGRATION.md).");
    static_assert(frontend::endsOnRequest<Frontend>,
                  "webfront::UIThread: this frontend cannot always be asked to close (the system browser waits for the console, "
                  "also as the fallback of NativeOrBrowserFrontend), so its thread could never be joined. With the system "
                  "browser no thread is needed: call start() on the application (docs/HOST_INTEGRATION.md).");

public:
    /// Constructs `App{config}` on a new thread, calls `setup(app)` there, then `app.show(page)`. See the class comment.
    template <typename Setup>
        requires std::invocable<Setup&, App&>
    explicit UIThread(WebFrontConfig config, Setup setup, std::string page = "index.html") {
        worker = std::thread([this, config = std::move(config), setup = std::move(setup), page = std::move(page)]() mutable {
            try {
                application.emplace(std::move(config));
                setup(*application);
            } catch (...) {
                application.reset();  // nobody saw it: it goes on the thread that made it
                settle(&UIThread::started, std::current_exception());
                return;
            }
            settle(&UIThread::started, nullptr);
            std::exception_ptr error;
            try {
                application->show(page);
            } catch (...) {
                error = std::current_exception();
            }
            settle(&UIThread::finished, std::move(error));
        });
        std::exception_ptr error;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return started; });
            if (!application)  // set before `started`: construction or setup failed
                error = std::exchange(failed, nullptr);
        }
        if (error) {
            worker.join();
            std::rethrow_exception(error);
        }
    }

    UIThread(const UIThread&)            = delete;
    UIThread& operator=(const UIThread&) = delete;

    /// Closes the window, joins the thread and destroys the application; a failure of show() is dropped. A destructor
    /// cannot leave its thread behind: it waits as long as the window takes to close (ADR-006). Call stop() first to be
    /// told about a window that does not.
    ~UIThread() {
        try {
            stop(std::chrono::milliseconds::max());
        } catch (...) {  // observed through failure() or an explicit stop(), or not at all
        }
        if (worker.joinable())  // never leave a joinable thread to std::thread's destructor, whatever stop() threw
            worker.join();
    }

    /// The application, valid until stop() or destruction. Use the calls that are callable from any thread.
    [[nodiscard]] App& app() noexcept {
        return *application;
    }

    /// True until show() has returned or thrown on the interface thread.
    [[nodiscard]] bool running() const {
        std::lock_guard lock(mutex);
        return !finished;
    }

    /// Waits for the end of show(); true when it ended within `timeout`.
    template <typename Rep, typename Period>
    bool waitFor(std::chrono::duration<Rep, Period> timeout) const {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return finished; });
    }

    /// What show() threw, once it has ended; null while it runs and after a normal end. stop() rethrows it.
    [[nodiscard]] std::exception_ptr failure() const {
        std::lock_guard lock(mutex);
        return finished ? failed : nullptr;
    }

    /// Asks the window to close without waiting (BasicWF::requestClose()); false when no window received the request.
    bool requestClose() {
        return application && application->requestClose();
    }

    /** @brief Ends the session and waits for it: asks the window to close until show() has returned (a window that is
     * still opening receives the request once it exists), joins the thread and destroys the application. Rethrows what
     * show() threw. Safe to repeat; afterwards app() is no longer valid.
     *
     * If show() has not returned within `patience` (`milliseconds::max()` waits without limit), throws UIThreadStalled
     * and changes nothing: the session still runs, app() stays valid and stop() can be called again. A request that
     * throws is treated like one that reached nothing and repeated; the thread is joined whatever the frontend does.
     */
    void stop(std::chrono::milliseconds patience = defaultPatience) {
        if (worker.joinable()) {
            const auto       start = std::chrono::steady_clock::now();
            std::unique_lock lock(mutex);
            while (!finished) {
                lock.unlock();
                try {
                    application->requestClose();
                } catch (...) {  // a request that failed is repeated at the next period, like one that reached nothing
                }
                lock.lock();
                if (changed.wait_for(lock, closeRetry, [&] { return finished; }))
                    break;
                // Elapsed time is converted down to the patience's unit: converting milliseconds::max() up would overflow.
                if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) >= patience)
                    throw UIThreadStalled();
            }
            lock.unlock();
            worker.join();
        }
        application.reset();
        std::exception_ptr error;
        {
            std::lock_guard lock(mutex);
            error = std::exchange(failed, nullptr);
        }
        if (error)
            std::rethrow_exception(error);
    }

    /// How long stop() waits for the window to close before reporting it.
    static constexpr std::chrono::milliseconds defaultPatience{std::chrono::seconds(30)};

private:
    // A request made before the window exists is not kept by the frontends: stop() repeats it at this period.
    static constexpr std::chrono::milliseconds closeRetry{10};

    // Records the end of a phase (and its failure) for the host thread.
    void settle(bool UIThread::* phase, std::exception_ptr error) {
        {
            std::lock_guard lock(mutex);
            failed       = std::move(error);
            this->*phase = true;
        }
        changed.notify_all();
    }

    std::optional<App>              application;  // written by the interface thread before `started`, then by stop()
    mutable std::mutex              mutex;        // guards started, finished and failed
    mutable std::condition_variable changed;
    bool                            started  = false;  // construction and setup are over, successfully or not
    bool                            finished = false;  // show() returned or threw
    std::exception_ptr              failed;
    std::thread                     worker;
};

}  // namespace webfront
