// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-03
//
// WebFront — frontend
// Role: Value types of the JavaScript dialogs (alert, confirm, prompt, beforeunload) an embedded frontend shows and answers.
// Thread: ui-loop | Alloc: heap-init
// LLM: data types only; the toolkit adapters fill and consume them

#pragma once

#include <functional>
#include <string>

namespace webfront::frontend {

/** @brief A JavaScript dialog the page asked an embedded frontend to show: `alert`, `confirm`, `prompt` or `beforeunload`. */
struct ScriptDialog {
    enum class Kind { alert, confirm, prompt, beforeUnload };

    Kind        kind{Kind::alert};
    std::string message{};      ///< The page's text; for `beforeunload`, whatever text the engine supplies (possibly empty, often a fixed phrase).
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
