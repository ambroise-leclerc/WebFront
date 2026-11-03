#pragma once

#include <functional>
#include <string>

namespace webfront::frontend {

/** @brief A JavaScript dialog the page asked an embedded frontend to show: `alert`, `confirm`, `prompt` or `beforeunload`. */
struct ScriptDialog {
    enum class Kind { alert, confirm, prompt, beforeUnload };

    Kind        kind{Kind::alert};
    std::string message{};      ///< The text of the page (empty for `beforeunload`, whose text the engine chooses).
    std::string defaultText{};  ///< The initial value of a `prompt`.
};

/** @brief The answer to a ScriptDialog. `accepted` is `confirm`'s result, `prompt`'s non-null result, and a `beforeunload` that lets the page go. */
struct ScriptDialogAnswer {
    bool        accepted{false};
    std::string text{};  ///< The string a `prompt` returns when accepted.
};

/**
 * @brief Test injection shared by the embedded frontends: answers script dialogs without showing a box.
 *
 * It runs on the thread of the frontend's window and must return promptly. When unset, each frontend shows the system's box
 * attached to its window; closing the window with a box open answers it as dismissed, so the window never blocks on it.
 */
using ScriptDialogHandler = std::function<ScriptDialogAnswer(const ScriptDialog&)>;

}  // namespace webfront::frontend
