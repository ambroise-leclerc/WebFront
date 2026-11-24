// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-19
//
// WebFront — core
// Role: Registers an application-chosen PTY for the optional xterm.js adapter.
// Thread: bridge-io | Alloc: unrestricted
// LLM: optional component; one session per name, retained across page reloads

#pragma once

#include "http/Encodings.hpp"
#include "Pty.hpp"
#include "Task.hpp"

#include <cmath>
#include <future>
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

    struct Launch {
        std::shared_future<std::shared_ptr<Pty>> terminal;
        std::uint32_t                            generation;
    };

    struct Session {
        PtyOptions              options;
        std::shared_ptr<Pty>    terminal{};
        std::shared_ptr<Launch> launching{};
        std::uint32_t           generation = 0;
        Pty&                    get(std::uint32_t expected) {
            if (!terminal || generation != expected)
                throw std::logic_error("PTY page session is stale or closed");
            return *terminal;
        }
    };
    auto state = std::make_shared<Session>(Session{std::move(options)});
    pty::detail::PageRegistration::addCleanup(app, [state] {
        state->launching.reset();  // A launch outlives disconnects and is joined at application shutdown.
        state->terminal.reset();
    });
    app.cppFunction(name + ".open", [state](typename App::UI ui, bool restart) -> ResultTask<std::uint32_t> {
        if (restart && (state->launching || (state->terminal && !state->terminal->exitCode())))
            throw std::logic_error("PTY page session is still running or launching");
        if (!state->launching) {
            if (state->terminal && !restart)
                co_return state->generation;
            if (state->generation == UINT32_MAX)
                throw std::overflow_error("PTY page generation exhausted");

            // Only the constructor runs off the server thread. Concurrent attachments share its result.
            auto launch        = std::make_shared<Launch>();
            launch->generation = state->generation + 1;
            launch->terminal   = std::async(std::launch::async, [options = state->options]() mutable {
                                   return std::make_shared<Pty>(std::move(options));
                                 }).share();
            state->launching   = std::move(launch);
        }
        auto launch = state->launching;
        using namespace std::chrono_literals;
        while (launch->terminal.wait_for(0ms) != std::future_status::ready)
            co_await ui.resumeAfter(10ms);

        try {
            auto terminal = launch->terminal.get();
            if (state->launching == launch) {
                state->terminal   = std::move(terminal);
                state->generation = launch->generation;
                state->launching.reset();
            }
        } catch (...) {
            if (state->launching == launch)
                state->launching.reset();
            throw;
        }
        co_return launch->generation;
    });
    using ReadResult = std::tuple<double, std::string, bool, bool, int>;
    app.cppFunction(name + ".read", [state](typename App::UI ui, std::uint32_t generation, double cursor) -> ResultTask<ReadResult> {
        if (!std::isfinite(cursor) || cursor < 0 || cursor > 9007199254740991.0 || std::trunc(cursor) != cursor)
            throw std::invalid_argument("Invalid PTY page cursor");
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        for (;;) {
            auto output = state->get(generation).tryRead(static_cast<std::uint64_t>(cursor));
            if (!output.bytes.empty() || output.exitCode || std::chrono::steady_clock::now() >= deadline)
                co_return ReadResult{static_cast<double>(output.next),
                                     base64::encode(std::move(output.bytes)),
                                     output.truncated,
                                     output.exitCode.has_value(),
                                     output.exitCode.value_or(0)};
            // Connection-owned timer. A browser reload stops this wait without blocking the server.
            co_await ui.resumeAfter(40ms);
        }
    });
    app.cppFunction(name + ".write", [state](std::uint32_t generation, const std::vector<std::uint8_t>& bytes) {
        return state->get(generation).write(bytes);
    });
    app.cppFunction(name + ".resize", [state](std::uint32_t generation, double columns, double rows) {
        const auto dimension = [](double value) {
            if (!(value >= 1 && value <= 32767) || std::trunc(value) != value)
                throw std::invalid_argument("Invalid PTY page dimension");
            return static_cast<std::uint16_t>(value);
        };
        state->get(generation).resize({dimension(columns), dimension(rows)});
    });
    app.cppFunction(name + ".hangUp", [state](std::uint32_t generation) {
        state->get(generation).hangUp();
    });
}
}  // namespace webfront
