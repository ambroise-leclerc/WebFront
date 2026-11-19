/// @brief Registers an application-chosen PTY for the optional xterm.js adapter.
#pragma once

#include "http/Encodings.hpp"
#include "Pty.hpp"

#include <cmath>
#include <memory>
#include <tuple>

namespace webfront {

namespace pty::detail {
struct PageRegistration {
    template <typename App, typename Cleanup>
    static void addCleanup(App& app, Cleanup&& cleanup) {
        app.ptyPageCleanups.emplace_back(std::forward<Cleanup>(cleanup));
    }
};
}  // namespace pty::detail

/// Call during application setup. Each name owns one session, retained across page reloads.
template <typename App>
void registerPtyPage(App& app, PtyOptions options, std::string name = "webfront.pty") {
    if (!app.allowsPtyPages())
        throw std::logic_error("PTY pages need WebFrontConfig::allowPtyPages");
    if (name.empty())
        throw std::invalid_argument("PTY page name cannot be empty");
    for (const auto* operation : {".open", ".read", ".write", ".resize", ".hangUp"})
        if (app.hasCppFunction(name + operation))
            throw std::invalid_argument("PTY page callback already registered: " + name + operation);

    struct Session {
        PtyOptions           options;
        std::unique_ptr<Pty> terminal{};
        Pty&                 get() {
            if (!terminal)
                throw std::logic_error("PTY page session is not open");
            return *terminal;
        }
    };
    auto state = std::make_shared<Session>(Session{std::move(options)});
    pty::detail::PageRegistration::addCleanup(app, [state] {
        state->terminal.reset();
    });
    app.cppFunction(name + ".open", [state] {
        if (!state->terminal)
            state->terminal = std::make_unique<Pty>(state->options);
    });
    app.cppFunction(name + ".read", [state](double cursor) {
        if (!std::isfinite(cursor) || cursor < 0 || cursor > 9007199254740991.0 || std::trunc(cursor) != cursor)
            throw std::invalid_argument("Invalid PTY page cursor");
        auto output = state->get().tryRead(static_cast<std::uint64_t>(cursor));
        return std::tuple{static_cast<double>(output.next),
                          base64::encode(std::move(output.bytes)),
                          output.truncated,
                          output.exitCode.has_value(),
                          output.exitCode.value_or(0)};
    });
    app.cppFunction(name + ".write", [state](const std::vector<std::uint8_t>& bytes) {
        return state->get().write(bytes);
    });
    app.cppFunction(name + ".resize", [state](double columns, double rows) {
        const auto dimension = [](double value) {
            if (!(value >= 1 && value <= 32767) || std::trunc(value) != value)
                throw std::invalid_argument("Invalid PTY page dimension");
            return static_cast<std::uint16_t>(value);
        };
        state->get().resize({dimension(columns), dimension(rows)});
    });
    app.cppFunction(name + ".hangUp", [state] {
        state->get().hangUp();
    });
}
}  // namespace webfront
