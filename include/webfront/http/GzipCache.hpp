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
 * @note Calls require serialization on the server context. Server worker results are published
 * here only after resuming on that context; workers never mutate the cache.
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
        if (auto cached = lookup(content, outputLimit, head))
            return cached->allowed ? std::optional{cached->length} : std::nullopt;
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

    struct Lookup {
        std::size_t length;
        bool        allowed;
    };

    /** @brief IO-context lookup only; a miss never decodes. */
    std::optional<Lookup> lookup(std::string& content, std::size_t outputLimit, bool head) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.encoded == content;
        });
        if (found == entries.end())
            return {};
        ++stats.hits;
        const auto length = found->decoded.size();
        if (length > outputLimit)
            return Lookup{length, false};
        entries.splice(entries.begin(), entries, found);
        if (head)
            content.clear();
        else
            content = found->decoded;
        return Lookup{length, true};
    }

    /** @brief Publish an already validated worker result on the IO context. */
    void remember(std::string_view encoded, std::string_view decoded) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.encoded == encoded;
        });
        if (found != entries.end())
            entries.splice(entries.begin(), entries, found);
        else
            retain(encoded, decoded);
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
