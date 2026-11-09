/// @brief Typed access to the native window of an embedded frontend (ADR-020, prerequisite of the parented native dialogs).
///
/// A frontend that owns a window publishes it in a `NativeWindowHost` while its loop runs: `publish()` once the window
/// exists, `retract()` before it is destroyed. Everything else reads it through a `NativeWindow` handle, whose `get()`
/// answers a `NativeWindowHandle` only
///  - while the window is published (before open() it is `notOpen`, after the loop ended it is `closed`), and
///  - on the interface thread of the frontend (ADR-006's executor): from any other thread it is `wrongThread`, because
///    a toolkit object may be used on that thread only.
/// The pointer is not owned and not reference counted: use it inside the interface-thread operation that asked for it
/// and never keep it. A window the user has just closed can still be published until the loop returns, so a consumer
/// that needs certainty checks the object with its toolkit (`IsWindow`, `-[NSWindow isVisible]`, `GTK_IS_WINDOW`).
/// The system browser has no such window: its frontends publish none.
#pragma once

#include "UIExecutor.hpp"

#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace webfront::frontend {

/// @brief The toolkit that owns the window; says how `NativeWindowHandle::pointer` is to be cast.
enum class NativeWindowKind {
    win32,  ///< `HWND` of the top-level window.
    cocoa,  ///< `NSWindow*`.
    gtk,    ///< `GtkWindow*` (GTK 4).
    cef,    ///< `CefBrowser*` of the application's browser (CEF Views: the window is reached through the browser view).
};

/// @brief Why a native window is not given.
enum class NativeWindowError {
    unsupported,  ///< The frontend has no native window (the system browser, a test double).
    notEmbedded,  ///< The connection is not the view of the frontend's window (a remote or system-browser client).
    notOpen,      ///< The window has not been created yet.
    closed,       ///< The loop has ended: the window is gone.
    wrongThread,  ///< Not called on the interface thread.
};

/// @brief The window as the toolkit knows it, valid on the interface thread while it is published.
struct NativeWindowHandle {
    NativeWindowKind kind{};
    /// `HWND`, `NSWindow*`, `GtkWindow*` or `CefBrowser*` according to `kind`; never null.
    void* pointer{};
    /// The `parent_window` identifier of the desktop portals: `"x11:<hex XID>"` or `"wayland:<exported handle>"` on Linux.
    /// Empty elsewhere, and on Linux while it is unknown (a Wayland handle is exported once the window is mapped).
    std::string portalParent;
};

using NativeWindowResult = std::expected<NativeWindowHandle, NativeWindowError>;

namespace detail {

class NativeWindowShared {
public:
    enum class Phase { idle, open, closed };

    void publish(NativeWindowHandle handle) {
        std::lock_guard lock(mutex);
        window = std::move(handle);
        phase  = Phase::open;
    }
    /// Sets the portal identifier of the published window (Wayland: known after the window is mapped); no-op otherwise.
    void setPortalParent(std::string portalParent) {
        std::lock_guard lock(mutex);
        if (phase == Phase::open)
            window.portalParent = std::move(portalParent);
    }
    void retract() noexcept {
        std::lock_guard lock(mutex);
        window = {};
        phase  = Phase::closed;
    }
    [[nodiscard]] NativeWindowResult get(const UIExecutor& executor) const {
        NativeWindowHandle copy;
        {
            std::lock_guard lock(mutex);
            if (phase == Phase::idle)
                return std::unexpected(NativeWindowError::notOpen);
            if (phase == Phase::closed)
                return std::unexpected(NativeWindowError::closed);
            copy = window;
        }
        if (!executor.onUIThread())
            return std::unexpected(NativeWindowError::wrongThread);
        return copy;
    }

private:
    mutable std::mutex mutex;
    Phase              phase{Phase::idle};
    NativeWindowHandle window;
};

}  // namespace detail

/// @brief Handle to the native window of a frontend. Cheap to copy; a default-constructed one has no window.
class NativeWindow {
    std::weak_ptr<detail::NativeWindowShared> shared;
    UIExecutor                                executor;

public:
    NativeWindow() = default;
    NativeWindow(std::weak_ptr<detail::NativeWindowShared> value, UIExecutor uiExecutor) : shared(std::move(value)), executor(std::move(uiExecutor)) { }

    /// The window, or why it is not given (see the file comment). Never blocks and never throws.
    [[nodiscard]] NativeWindowResult get() const {
        const auto state = shared.lock();
        if (!state)
            return std::unexpected(NativeWindowError::unsupported);
        return state->get(executor);
    }
};

/// @brief The frontend's side: publishes the window it created, from the interface thread, and retracts it before it dies.
class NativeWindowHost {
    std::shared_ptr<detail::NativeWindowShared> shared{std::make_shared<detail::NativeWindowShared>()};
    UIExecutor                                  executor;

public:
    explicit NativeWindowHost(UIExecutor uiExecutor) : executor(std::move(uiExecutor)) { }

    [[nodiscard]] NativeWindow window() const {
        return NativeWindow{shared, executor};
    }
    /// Gives the window to consumers. Call it on the interface thread, once the window exists.
    void publish(NativeWindowHandle handle) {
        shared->publish(std::move(handle));
    }
    void setPortalParent(std::string portalParent) {
        shared->setPortalParent(std::move(portalParent));
    }
    /// Withdraws the window; call it before the toolkit destroys it. Idempotent. A later publish() opens a new session.
    void retract() noexcept {
        shared->retract();
    }
};

/// @brief The process-wide window host of one frontend type (`Tag` is the frontend's own type), next to its executor host.
template <typename Tag>
[[nodiscard]] NativeWindowHost& nativeWindowHostOf() {
    static NativeWindowHost host{uiExecutorHostOf<Tag>().executor()};
    return host;
}

}  // namespace webfront::frontend
