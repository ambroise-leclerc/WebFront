// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: A registry of watches with one connection, and readers of the pages, for the tests of the inspectors' trees (ADR-027).
// Thread: worker-safe | Alloc: unrestricted
// LLM: test helper of WatchTreeTests.cpp, WatchPassTests.cpp and WatchWriteTests.cpp; no network, the operations are called directly

#pragma once

#include "FormsSettings.hpp"
#include <webfront/BridgeError.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/Watches.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// The fixture of the forms is described for the inspectors too.
template <>
struct webfront::inspect::Described<forms_fixture::Settings> {
    static webfront::forms::Description<forms_fixture::Settings> description() {
        return forms_fixture::settingsDescription();
    }
};

namespace watch_fixture {

using webfront::inspect::Limits;
using webfront::inspect::Node;
using webfront::inspect::Page;
using webfront::inspect::Visit;
using webfront::inspect::WatchRegistry;
namespace flag = webfront::inspect::flag;
namespace code = webfront::inspect::code;

inline const std::string& pathOf(const Node& node) {
    return std::get<0>(node);
}
inline const std::string& kindOf(const Node& node) {
    return std::get<1>(node);
}
inline const std::string& labelOf(const Node& node) {
    return std::get<2>(node);
}
inline const std::string& valueOf(const Node& node) {
    return std::get<3>(node);
}
inline std::size_t countOf(const Node& node) {
    return static_cast<std::size_t>(std::get<4>(node));
}
inline unsigned flagsOf(const Node& node) {
    return static_cast<unsigned>(std::get<5>(node));
}
inline const std::vector<Node>& nodesOf(const Page& page) {
    return std::get<0>(page);
}
inline double nextOf(const Page& page) {
    return std::get<1>(page);
}
inline double revisionOf(const Page& page) {
    return std::get<2>(page);
}
inline bool resyncOf(const Page& page) {
    return std::get<3>(page);
}
inline std::vector<std::string> pathsOf(const Page& page) {
    std::vector<std::string> paths;
    for (const auto& node : nodesOf(page))
        paths.push_back(pathOf(node));
    return paths;
}
inline std::vector<std::string> valuesOf(const Page& page) {
    std::vector<std::string> values;
    for (const auto& node : nodesOf(page))
        values.push_back(valueOf(node));
    return values;
}
/// The node of `path` in the page; fails the scenario when it is not there.
inline const Node& nodeAt(const Page& page, std::string_view path) {
    for (const auto& node : nodesOf(page))
        if (pathOf(node) == path)
            return node;
    webfront::bdd::require(false);
    return nodesOf(page).front();
}
inline bool hasNode(const Page& page, std::string_view path) {
    for (const auto& node : nodesOf(page))
        if (pathOf(node) == path)
            return true;
    return false;
}
/// The bytes a page takes in its message, header included.
inline std::size_t bytesOf(const Page& page) {
    auto bytes = webfront::inspect::detail::pageOverheadBytes;
    for (const auto& node : nodesOf(page))
        bytes += webfront::inspect::detail::nodeBytes(node);
    return bytes;
}

/// The code of the BridgeError that `call` throws, "" when it throws nothing, "exception" for another exception.
template <typename Call>
std::string codeOf(Call&& call) {
    try {
        static_cast<void>(call());
    } catch (const webfront::BridgeError& error) {
        return error.code();
    } catch (const std::exception&) {
        return "exception";
    }
    return {};
}
template <typename Call>
std::string messageOf(Call&& call) {
    try {
        static_cast<void>(call());
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

/// A visitor source of `object`, as an application writes one (here without a lock: one thread).
template <typename T>
auto visiting(T& object) {
    return [&object](Visit<T> visit) {
        visit(object);
    };
}

/// A registry, the session of one connection, and the calls of the inspectors on it.
struct Bench {
    std::shared_ptr<WatchRegistry>                   registry = std::make_shared<WatchRegistry>();
    std::shared_ptr<webfront::inspect::WatchSession> session;
    Limits                                           limits{.responseBudget = 32768, .maxWriteTextBytes = 4096};
    std::mutex                                       diagnosticsMutex;
    std::vector<std::string>                         diagnostics;

    Bench() {
        registry->diagnostic = [this](std::string_view message) {
            std::scoped_lock lock{diagnosticsMutex};
            diagnostics.emplace_back(message);
        };
        session = registry->session(1);
    }

    template <typename Source>
    std::uint64_t watch(std::string name, Source&& source) {
        using Shape = webfront::inspect::SourceShape<std::decay_t<Source>>;
        return registry->add(webfront::inspect::makeWatch<Shape>(std::move(name),
                                                                 "test.cpp:1",
                                                                 std::forward<Source>(source),
                                                                 std::shared_ptr<webfront::inspect::NoState>{},
                                                                 webfront::inspect::NoExecutor{},
                                                                 webfront::inspect::NoMutator{}));
    }
    template <typename Source, typename Mutator>
    std::uint64_t watch(std::string name, Source&& source, Mutator mutator) {
        using Shape = webfront::inspect::SourceShape<std::decay_t<Source>>;
        return registry->add(webfront::inspect::makeWatch<Shape>(std::move(name),
                                                                 "test.cpp:1",
                                                                 std::forward<Source>(source),
                                                                 std::shared_ptr<webfront::inspect::NoState>{},
                                                                 webfront::inspect::NoExecutor{},
                                                                 std::move(mutator)));
    }
    template <typename State, typename Source>
    std::uint64_t owning(std::string name, std::shared_ptr<State> state, Source&& source) {
        using Shape = webfront::inspect::OwnedSourceShape<std::decay_t<Source>, State>;
        return registry->add(webfront::inspect::makeWatch<Shape>(std::move(name),
                                                                 "test.cpp:1",
                                                                 std::forward<Source>(source),
                                                                 std::move(state),
                                                                 webfront::inspect::NoExecutor{},
                                                                 webfront::inspect::NoMutator{}));
    }

    std::size_t diagnosticCount() {
        std::scoped_lock lock{diagnosticsMutex};
        return diagnostics.size();
    }
    std::string lastDiagnostic() {
        std::scoped_lock lock{diagnosticsMutex};
        return diagnostics.empty() ? std::string{} : diagnostics.back();
    }

    double reset(std::uint64_t watch) {
        return registry->reset(session, static_cast<double>(watch));
    }
    Page children(std::uint64_t watch, std::string_view path, double revision, double start = 0, double count = 200) {
        return registry->children(session, static_cast<double>(watch), path, revision, start, count, limits);
    }
    Page changes(std::uint64_t watch, double revision, double cursor = 0, double count = 200) {
        return registry->changes(session, static_cast<double>(watch), revision, cursor, count, limits);
    }
    Page fold(std::uint64_t watch, std::string_view path, double revision) {
        return registry->fold(session, static_cast<double>(watch), path, revision);
    }
    webfront::inspect::WriteAnswer write(std::uint64_t watch, double revision, std::string_view path, std::string_view expected, std::string_view text) {
        return registry->write(session, static_cast<double>(watch), revision, path, expected, text, limits);
    }
    webfront::inspect::ValueAnswer value(std::uint64_t watch, std::string_view path, double snapshot = 0, double offset = 0) {
        return registry->value(session, static_cast<double>(watch), path, snapshot, offset, limits);
    }
    /// The memory of the watch for the bench's connection.
    std::shared_ptr<webfront::inspect::detail::Memory> memory(std::uint64_t watch) {
        return session->memory(watch);
    }
    /// Opens a watch as the panel does: an empty memory, then the root and its first children. Returns that page.
    Page open(std::uint64_t watch, double count = 200) {
        return children(watch, "", reset(watch), 0, count);
    }
};

}  // namespace watch_fixture
