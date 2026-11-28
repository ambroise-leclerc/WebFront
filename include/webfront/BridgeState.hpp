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
#include <any>
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
/// The states of at most this many pages are kept: past it, the state of the page read the longest ago is dropped, and that
/// page publishes it again when it is read.
inline constexpr std::size_t maxBridgeSources = 16;
/// At most this many pages are known at once, each by its identifier and the two callbacks of its connection. A connection
/// has one page at most (announcing another identifier drops its previous one), and a page whose connection is gone is
/// dropped first: past the bound, an announcement is refused, and no connected page ever loses its place.
inline constexpr std::size_t maxBridgeAnnouncements = 256;
/// A page publishes for this long after an inspector read its state: longer than the slowest period of a panel (5 s), so
/// that a panel that reads keeps its page publishing, and a panel that is paused or closed lets it stop.
inline constexpr std::chrono::seconds bridgeLease{15};
/// A page that should publish and did not for this long is asked again, at most once per such duration.
inline constexpr std::chrono::seconds bridgeAskAgain{1};
/// The cursors of the pages of a state count rows in units of this, more than a state holds (maxBridgeRows).
inline constexpr std::uint64_t bridgeCursorStride = 1024;

/// A row of the state of a bridge, as the page writes it (inspector-bridge.mjs): (kind, name, detail, number, number).
/// The kinds are the page's vocabulary ("page", "ready", "connection", "counts", "pending", "rejected", "js"): C++
/// relays the rows without reading them, and only bounds them.
using BridgeRow = std::tuple<std::string, std::string, std::string, double, double>;
/// A page of the state: (rows, next, publication, age in milliseconds, flags). `next` is the cursor of the following page,
/// 0 at the end: it names the publication the first page read and the place in it, so that the following pages read the
/// same publication even after a newer one (the previous one is kept). `publication` numbers the state the rows belong to,
/// 0 before the first one: a reader that sees it change between two pages (that publication is gone) starts again. The
/// flags are those of bridge_flag.
/// A cursor is publication × bridgeCursorStride + index; a cursor below the stride is an index in the latest publication.
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
     * The page `host` announces that it can publish the state of its bridge: `owner` identifies its connection (a value
     * that compares equal only for the same one), `ask` asks it to start (called on a reader's thread, outside the lock),
     * `alive` tells whether its connection is still there and admitted. The same connection announcing itself again (its
     * Bridge panel was detached again) keeps its state; another connection under the same identifier (the page was loaded
     * again) replaces it and its state is forgotten. A reader's lease is kept either way. Returns whether a reader is
     * waiting for it now.
     * @throws BridgeError `webfront.inspector.unavailable` when maxBridgeAnnouncements connected pages are known already
     */
    template <typename Owner>
    bool announce(const std::string& host, Owner owner, std::function<void()> ask, std::function<bool()> alive, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        // Dropped: the pages whose connection is gone (a reader's lease is kept for the page that comes back under that
        // identifier), and the earlier page of this connection under another identifier.
        std::erase_if(sources, [&](auto& entry) {
            if (entry.first == host)
                return false;
            const auto same = std::any_cast<Owner>(&entry.second.owner);
            if (same && *same == owner)
                return true;
            if (entry.second.alive())
                return false;
            if (now < entry.second.wantedUntil)
                keepReader(entry.first, entry.second.wantedUntil);
            return true;
        });
        if (!sources.contains(host) && sources.size() >= maxBridgeAnnouncements)
            throw webfront::detail::makeBridgeError(code::unavailable, "Too many pages publish the state of their bridge");
        // A reader may have asked before the page announced itself (a detached panel outlives a reload of its page).
        auto wanted = readers.contains(host) ? readers[host] : Clock::time_point{};
        readers.erase(host);
        auto& source    = sources[host];
        wanted          = (std::max)(wanted, source.wantedUntil);  // a source that announces itself again keeps its reader
        const auto same = std::any_cast<Owner>(&source.owner);
        if (!same || !(*same == owner)) {
            source       = Source{};
            source.owner = std::move(owner);
        }
        source.ask         = std::move(ask);
        source.alive       = std::move(alive);
        source.wantedUntil = wanted;
        return now < source.wantedUntil;
    }

    /**
     * The page `host` publishes the state of its bridge. Each text is made safe and bounded (boundedText). Returns
     * whether a reader still asks for it: the page stops publishing when not.
     * @throws std::invalid_argument for more than maxBridgeRows rows; nothing is kept then
     * @throws BridgeError `webfront.inspector.unknown` when the page did not announce itself (or lost its place: it
     *         announces itself again), `webfront.inspector.unavailable` when another connection announced itself under
     *         this identifier since: the state shown is that of the latest one only
     */
    template <typename Owner>
    bool publish(const std::string& host, const Owner& owner, std::vector<BridgeRow> rows, Clock::time_point now = Clock::now()) {
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
        auto&      source = found->second;
        const auto holder = std::any_cast<Owner>(&source.owner);
        if (!holder || !(*holder == owner))
            throw webfront::detail::makeBridgeError(code::unavailable, "Another page publishes the state of its bridge under this identifier");
        // The previous publication stays readable, so that a reader that began it reads it to its end.
        source.previousRows        = std::exchange(source.rows, std::move(rows));
        source.previousPublication = source.publication;
        source.previousPublishedAt = source.publishedAt;
        source.publication         = ++publications;
        source.publishedAt         = now;
        const bool wanted          = now < source.wantedUntil;
        // The rows of at most maxBridgeSources pages are kept: the page read the longest ago loses its state, not its place,
        // so that a read finds it, asks it and gets its state again.
        std::size_t holding = 0;
        for (const auto& entry : sources)
            holding += entry.second.publication != 0 ? 1 : 0;
        while (holding > maxBridgeSources) {
            Source* oldest = nullptr;
            for (auto& entry : sources)
                if (entry.second.publication != 0 && (!oldest || entry.second.wantedUntil < oldest->wantedUntil))
                    oldest = &entry.second;
            if (!oldest)
                break;
            oldest->rows                = {};
            oldest->previousRows        = {};
            oldest->publication         = 0;
            oldest->previousPublication = 0;
            --holding;
        }
        return wanted;
    }

    /**
     * The page that webfront.inspector.bridge(host, from, count) answers: the rows from the cursor `from` (0 for the first
     * page of the latest publication, else the `next` of the page before), at most `count` and as many as fit `budget`.
     * A cursor of a publication that is neither the latest nor the one before gives an empty last page under the latest
     * publication, which the reader sees changed. A read renews the lease of the source, and gives the request to make to
     * a source that is not publishing.
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
            // Kept for the page that announces itself next under this identifier.
            keepReader(host, now + bridgeLease);
            result.page = BridgePage{std::vector<BridgeRow>{}, 0.0, 0.0, 0.0, 0.0};
            return result;
        }
        auto& source = found->second;
        // The latest lease wins: a read whose time was taken earlier but that takes the lock last never shortens it.
        source.wantedUntil = (std::max)(source.wantedUntil, now + bridgeLease);
        const bool fresh   = source.publication != 0 && now - source.publishedAt < bridgeAskAgain;
        if (!fresh && (source.askedAt == Clock::time_point{} || now - source.askedAt >= bridgeAskAgain)) {
            source.askedAt = now;
            result.ask     = source.ask;
        }
        // The publication the cursor names: the latest, the one before it, or one that is gone (nothing to serve).
        const auto                          named       = first / bridgeCursorStride;
        const auto*                         state       = &source.rows;
        auto                                served      = source.publication;
        auto                                publishedAt = source.publishedAt;
        std::uint64_t                       start       = first % bridgeCursorStride;
        static const std::vector<BridgeRow> none;
        if (named != 0 && named != source.publication) {
            if (named == source.previousPublication) {
                state       = &source.previousRows;
                served      = source.previousPublication;
                publishedAt = source.previousPublishedAt;
            } else
                state = &none;
        }
        std::vector<BridgeRow> rows;
        std::size_t            used = 0;
        double                 next = 0;
        for (std::size_t index = static_cast<std::size_t>(std::min<std::uint64_t>(start, state->size())); index < state->size(); ++index) {
            const auto& row   = (*state)[index];
            const auto  bytes = detail::encodedTuple + 2 * detail::encodedNumber + detail::encodedText(std::get<0>(row).size())
                                + detail::encodedText(std::get<1>(row).size()) + detail::encodedText(std::get<2>(row).size());
            if (rows.size() == wanted || used + bytes > room) {
                next = static_cast<double>(served * bridgeCursorStride + index);
                break;
            }
            used += bytes;
            rows.push_back(row);
        }
        // `now` may have been read before a publication that took the lock first: an age is never negative.
        const auto age = served == 0 ? 0.0 : (std::max)(0.0, std::chrono::duration<double, std::milli>(now - publishedAt).count());
        result.page    = BridgePage{std::move(rows),
                                    next,
                                    static_cast<double>(served),
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
    /// Keeps a reader's lease for an identifier that no page holds now: the latest of the leases, within the bound of
    /// maxBridgeSources identifiers (past it, the one whose lease ends first is dropped).
    void keepReader(const std::string& host, Clock::time_point until) {
        if (const auto found = readers.find(host); found != readers.end()) {
            found->second = (std::max)(found->second, until);
            return;
        }
        if (readers.size() >= maxBridgeSources)
            readers.erase(std::ranges::min_element(readers, {}, [](const auto& entry) {
                return entry.second;
            }));
        readers.emplace(host, until);
    }

    struct Source {
        std::any               owner;
        std::function<void()>  ask;
        std::function<bool()>  alive;
        std::vector<BridgeRow> rows;
        std::uint64_t          publication = 0;
        Clock::time_point      publishedAt;
        std::vector<BridgeRow> previousRows;  // the publication before, for a reader that began it
        std::uint64_t          previousPublication = 0;
        Clock::time_point      previousPublishedAt;
        Clock::time_point      wantedUntil;
        Clock::time_point      askedAt;
    };

    mutable std::mutex                       mutex;
    std::map<std::string, Source>            sources;
    std::map<std::string, Clock::time_point> readers;  // the leases of the identifiers read before their page announced itself
    std::uint64_t                            publications = 0;
};

}  // namespace webfront::inspect
