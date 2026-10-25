/** @brief Application-owned, bounded LRU of fully validated gzip identity representations. */
#pragma once

#include "Gzip.hpp"

#include <algorithm>
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace webfront::http::gzip {

/**
 * @brief Serial-context cache keyed by complete compressed bytes, independent of filesystem paths.
 * @note Like RequestHandler, calls require serialization on the server context. A cold decode
 * finishes before another request runs, so queued misses for the same bytes reuse its result.
 * Copies start empty; no cache storage is implicitly shared between applications.
 */
class ValidatedCache {
public:
    struct Statistics {
        std::size_t decodes{};
        std::size_t hits{};
        std::size_t entries{};
        std::size_t retainedBytes{};
    };

    explicit ValidatedCache(std::size_t cacheBudget) : budget(cacheBudget) {}
    ValidatedCache(const ValidatedCache& other) : budget(other.budget) {}
    ValidatedCache& operator=(const ValidatedCache& other) {
        if (this != &other) {
            entries.clear();
            stats  = {};
            budget = other.budget;
        }
        return *this;
    }
    ValidatedCache(ValidatedCache&& other) noexcept : budget(other.budget), entries(std::move(other.entries)), stats(std::exchange(other.stats, {})) {}
    ValidatedCache& operator=(ValidatedCache&& other) noexcept {
        if (this != &other) {
            entries = std::move(other.entries);
            budget  = other.budget;
            stats   = std::exchange(other.stats, {});
        }
        return *this;
    }

    /** @brief Replaces compressed content with identity (or clears it for HEAD), returning validated length. */
    std::optional<std::size_t> resolve(std::string& content, std::size_t outputLimit, bool head = false) {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.encoded == content;
        });
        if (found != entries.end()) {
            ++stats.hits;
            // Re-check the current policy even when a larger budget validated this revision earlier.
            if (found->decoded.size() > outputLimit)
                return {};
            entries.splice(entries.begin(), entries, found);
            const auto length = found->decoded.size();
            if (head)
                content.clear();
            else
                content = found->decoded;
            return length;
        }
        ++stats.decodes;
        auto decoded = decodeGzip(content, outputLimit);
        if (!decoded)
            return {};
        const auto length = decoded->size();
        retain(content, *decoded);
        if (head)
            content.clear();
        else
            content = std::move(*decoded);
        return length;
    }

    [[nodiscard]] Statistics statistics() const {
        auto result    = stats;
        result.entries = entries.size();
        return result;
    }

private:
    struct Entry {
        std::string encoded;
        std::string decoded;
        std::size_t charge;
    };
    // Includes string capacities/terminators, entry metadata and the two list links.
    // Allocator bookkeeping is implementation-defined and excluded.
    static constexpr std::size_t overhead = sizeof(Entry) + 2 * sizeof(void*) + 2;
    std::size_t                  budget;
    std::list<Entry>             entries;
    Statistics                   stats;

    void retain(std::string_view encoded, std::string_view decoded) {
        // Oversized results do not evict useful entries or allocate a retained copy.
        if (budget < overhead || encoded.size() > budget - overhead || decoded.size() > budget - overhead - encoded.size())
            return;
        Entry entry{std::string(encoded), std::string(decoded), 0};
        if (entry.encoded.capacity() > budget - overhead || entry.decoded.capacity() > budget - overhead - entry.encoded.capacity())
            return;
        entry.charge = overhead + entry.encoded.capacity() + entry.decoded.capacity();
        while (stats.retainedBytes > budget - entry.charge) {
            stats.retainedBytes -= entries.back().charge;
            entries.pop_back();
        }
        entries.push_front(std::move(entry));
        stats.retainedBytes += entries.front().charge;
    }
};

}  // namespace webfront::http::gzip
