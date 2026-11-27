// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — core
// Role: Relay of the state of the bridge in a page (ADR-027, section 8): the page publishes it, a detached inspector reads it.
// Thread: thread-safe | Alloc: heap-init
// LLM: internal; a page publishes only while a reader asks (the lease), keep every stored text and count bounded

#pragma once

#include "BridgeError.hpp"
#include "Introspection.hpp"
#include "Watches.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace webfront::inspect {

/// A published state holds at most this many rows; a publication of more is refused.
inline constexpr std::size_t maxBridgeRows = 256;
/// The states of at most this many pages are kept; a page that announces itself past it takes the place of the one that
/// was read the longest ago.
inline constexpr std::size_t maxBridgeSources = 16;
/// A page publishes for this long after an inspector read its state: longer than the slowest period of a panel (5 s), so
/// that a panel that reads keeps its page publishing, and a panel that is paused or closed lets it stop.
inline constexpr std::chrono::seconds bridgeLease{15};
/// A page that should publish and did not for this long is asked again, at most once per such duration.
inline constexpr std::chrono::seconds bridgeAskAgain{1};

/// A row of the state of a bridge, as the page writes it (inspector-bridge.mjs): (kind, name, detail, number, number).
/// The kinds are the page's vocabulary ("page", "ready", "connection", "counts", "pending", "rejected", "js"): C++
/// relays the rows without reading them, and only bounds them.
using BridgeRow = std::tuple<std::string, std::string, std::string, double, double>;
/// A page of the state: (rows, next, publication, age in milliseconds, flags). `next` is the index of the first row of
/// the following page, 0 at the end. `publication` numbers the state the rows belong to, 0 before the first one: a reader
/// that sees it change between two pages starts again. The flags are those of bridge_flag.
using BridgePage = std::tuple<std::vector<BridgeRow>, double, double, double, double>;

/// The bits of the flags of a BridgePage.
namespace bridge_flag {
inline constexpr unsigned source    = 1;  ///< the page of that identifier announced itself and is still connected
inline constexpr unsigned published = 2;  ///< it published a state at least once
}  // namespace bridge_flag

inline constexpr std::size_t maxBridgeRowBytes = detail::encodedTuple + 2 * detail::encodedNumber + detail::encodedText(maxKindBytes)
                                                 + 2 * detail::encodedText(maxTextBytes);
static_assert(maxBridgeRowBytes + responseHeaderBytes <= minPageBytes);

/**
 * @brief The states of the bridges that pages publish for the inspectors that cannot read them directly: a detached
 * panel is another window, which does not see the page of the application (ADR-027, section 8). A page that holds the
 * inspectors announces itself under its host identifier; it publishes only while a reader asks, so that a page nobody
 * inspects sends nothing. Thread-safe. A source's `ask` is called outside the lock, by whoever read; its `alive` is called
 * under it and must not come back here.
 */
class BridgeStates {
public:
    using Clock = std::chrono::steady_clock;

    /// What a read gives beside its page: the request to make to the source (null when none is due), outside any lock.
    struct Read {
        BridgePage            page;
        std::function<void()> ask;
    };

    /**
     * The page `host` announces that it can publish the state of its bridge: `ask` asks it to start (called on a
     * reader's thread, outside the lock), `alive` tells whether its connection is still there and admitted. An earlier source of the
     * same identifier (the page was loaded again) is replaced and its state forgotten. Returns whether a reader is
     * waiting for it now.
     */
    bool announce(const std::string& host, std::function<void()> ask, std::function<bool()> alive, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        std::erase_if(sources, [&](const auto& entry) {
            return entry.first != host && !entry.second.alive();
        });
        if (!sources.contains(host) && sources.size() >= maxBridgeSources)
            sources.erase(std::ranges::min_element(sources, {}, [](const auto& entry) {
                return entry.second.wantedUntil;
            }));
        // A reader may have asked before the page announced itself (a detached panel outlives a reload of its page).
        const auto wanted = readers.contains(host) ? readers[host] : Clock::time_point{};
        readers.erase(host);
        auto& source       = sources[host];
        source             = Source{};
        source.ask         = std::move(ask);
        source.alive       = std::move(alive);
        source.wantedUntil = wanted;
        return now < source.wantedUntil;
    }

    /**
     * The page `host` publishes the state of its bridge. Each text is made safe and bounded (boundedText). Returns
     * whether a reader still asks for it: the page stops publishing when not.
     * @throws std::invalid_argument for more than maxBridgeRows rows; nothing is kept then
     * @throws BridgeError `webfront.inspector.unknown` when the page did not announce itself
     */
    bool publish(const std::string& host, std::vector<BridgeRow> rows, Clock::time_point now = Clock::now()) {
        if (rows.size() > maxBridgeRows)
            throw std::invalid_argument("The state of a bridge holds at most " + std::to_string(maxBridgeRows) + " rows");
        for (auto& [kind, name, text, first, second] : rows) {
            kind = boundedText(kind, maxKindBytes);
            name = boundedText(name);
            text = boundedText(text);
        }
        std::scoped_lock lock{mutex};
        const auto       found = sources.find(host);
        if (found == sources.end())
            throw webfront::detail::makeBridgeError(code::unknown, "This page did not announce the state of its bridge");
        auto& source       = found->second;
        source.rows        = std::move(rows);
        source.publication = ++publications;
        source.publishedAt = now;
        return now < source.wantedUntil;
    }

    /**
     * The page that webfront.inspector.bridge(host, from, count) answers: the rows of the last publication from index
     * `from`, at most `count` and as many as fit `budget`. A read renews the lease of the source, and gives the request
     * to make to a source that is not publishing.
     */
    [[nodiscard]] Read read(const std::string& host, double from, double count, std::size_t budget, Clock::time_point now = Clock::now()) {
        const auto       first  = detail::pageNumber(from, "cursor");
        const auto       wanted = std::min<std::uint64_t>(detail::pageNumber(count, "count"), maxPageCount);
        const auto       room   = detail::rowsRoom(budget);
        Read             result;
        std::scoped_lock lock{mutex};
        const auto       found = sources.find(host);
        if (found == sources.end() || !found->second.alive()) {
            if (found != sources.end())
                sources.erase(found);
            // Kept for the page that announces itself next under this identifier, within the same bound as the sources.
            if (readers.size() >= maxBridgeSources && !readers.contains(host))
                readers.erase(std::ranges::min_element(readers, {}, [](const auto& entry) {
                    return entry.second;
                }));
            readers[host] = now + bridgeLease;
            result.page   = BridgePage{std::vector<BridgeRow>{}, 0.0, 0.0, 0.0, 0.0};
            return result;
        }
        auto& source       = found->second;
        source.wantedUntil = now + bridgeLease;
        const bool fresh   = source.publication != 0 && now - source.publishedAt < bridgeAskAgain;
        if (!fresh && (source.askedAt == Clock::time_point{} || now - source.askedAt >= bridgeAskAgain)) {
            source.askedAt = now;
            result.ask     = source.ask;
        }
        std::vector<BridgeRow> rows;
        std::size_t            used = 0;
        double                 next = 0;
        for (std::size_t index = static_cast<std::size_t>(std::min<std::uint64_t>(first, source.rows.size())); index < source.rows.size(); ++index) {
            const auto& row   = source.rows[index];
            const auto  bytes = detail::encodedTuple + 2 * detail::encodedNumber + detail::encodedText(std::get<0>(row).size())
                                + detail::encodedText(std::get<1>(row).size()) + detail::encodedText(std::get<2>(row).size());
            if (rows.size() == wanted || used + bytes > room) {
                next = static_cast<double>(index);
                break;
            }
            used += bytes;
            rows.push_back(row);
        }
        const auto age = source.publication == 0 ? 0.0 : std::chrono::duration<double, std::milli>(now - source.publishedAt).count();
        result.page    = BridgePage{std::move(rows),
                                    next,
                                    static_cast<double>(source.publication),
                                    age,
                                    static_cast<double>(bridge_flag::source | (source.publication != 0 ? bridge_flag::published : 0u))};
        return result;
    }

    /// The pages that announced themselves and were not replaced or found gone.
    [[nodiscard]] std::size_t sourceCount() const {
        std::scoped_lock lock{mutex};
        return sources.size();
    }

private:
    struct Source {
        std::function<void()>  ask;
        std::function<bool()>  alive;
        std::vector<BridgeRow> rows;
        std::uint64_t          publication = 0;
        Clock::time_point      publishedAt;
        Clock::time_point      wantedUntil;
        Clock::time_point      askedAt;
    };

    mutable std::mutex                       mutex;
    std::map<std::string, Source>            sources;
    std::map<std::string, Clock::time_point> readers;  // the leases of the identifiers read before their page announced itself
    std::uint64_t                            publications = 0;
};

}  // namespace webfront::inspect
