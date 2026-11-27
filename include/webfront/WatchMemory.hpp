// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — core
// Role: What one connection knows of one watch (ADR-027, section 3): the nodes sent, their pages, the passes of differences and the writes.
// Thread: thread-safe | Alloc: unrestricted
// LLM: internal; one Memory per connection and watch, each operation whole inside the access window the application defines

#pragma once

#include "WatchValues.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace webfront::inspect::detail {

// --- Encoded sizes (weblink/Messages.hpp) ----------------------------------------------------------------------

/// The bytes a string takes on the wire: its code, its length on 1, 2 or 4 bytes, and its bytes.
[[nodiscard]] constexpr std::size_t stringBytes(std::size_t size) noexcept {
    return (size < 256 ? 2u : size <= 65535 ? 3u : 5u) + size;
}
/// The bytes of a node: a tuple header, four strings and two numbers.
[[nodiscard]] inline std::size_t nodeBytes(const Node& node) noexcept {
    return 2 + stringBytes(std::get<0>(node).size()) + stringBytes(std::get<1>(node).size()) + stringBytes(std::get<2>(node).size())
           + stringBytes(std::get<3>(node).size()) + 9 + 9;
}
/// The bytes of a page besides its nodes: the message header, the tuple and list headers, two numbers and a boolean.
inline constexpr std::size_t pageOverheadBytes = 8 + 2 + 5 + 9 + 9 + 1;
static_assert(pageOverheadBytes <= responseHeaderBytes);
/// The bytes of a node whose every field is at its bound.
inline constexpr std::size_t largestNodeBytes = 2 + (3 + maxPathBytes) + (2 + maxKindBytes) + 2 * (3 + maxTextBytes) + 9 + 9;
static_assert(largestNodeBytes <= maxNodeBytes);
static_assert(pageOverheadBytes + 2 * largestNodeBytes <= minPageBytes, "The smallest page holds a parent and one child");
/// The answer of a write, at its largest: the header, a tuple, a number, the longest code and a message of 256 bytes.
static_assert(8 + 2 + 9 + (2 + 64) + (3 + maxTextBytes) <= minPageBytes);

[[nodiscard]] inline BridgeError refusal(std::string_view code, std::string_view message) {
    return webfront::detail::makeBridgeError(code, std::string(message));
}

/// A number of the protocol: a whole number from 0 to 2^53.
[[nodiscard]] inline std::uint64_t wholeNumber(double value, std::string_view what) {
    if (!std::isfinite(value) || value < 0 || value > maxExactNumber || std::trunc(value) != value)
        throw std::invalid_argument("The inspectors' " + std::string(what) + " must be a whole number from 0 to 2^53");
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] inline unsigned flagsOf(const Node& node) noexcept {
    return static_cast<unsigned>(std::get<5>(node));
}
[[nodiscard]] inline std::size_t childrenOf(const Node& node) noexcept {
    return static_cast<std::size_t>(std::get<4>(node));
}

/// The text of an exception of the application's code, as the value of a node.
[[nodiscard]] inline std::string exceptionValue(std::string_view what) {
    static constexpr std::string_view open  = "\xE2\x80\xB9"
                                              "exception : ",
                                      close = "\xE2\x80\xBA";
    return std::string(open) + boundedText(what, maxTextBytes - open.size() - close.size()) + std::string(close);
}

/// A parent whose children could not all have a path within the bounds is not unfolded (ADR-027, section 3).
[[nodiscard]] inline bool depthReached(std::string_view path, const NodeOps& ops) noexcept {
    const auto level = static_cast<std::size_t>(std::ranges::count(path, '/'));
    return ops.container && (level >= maxPathSegments || path.size() + 1 + (ops.keyed ? maxKeySegment : maxIndexSegment) > maxPathBytes);
}

/// A node read in the object, with what the memory keeps to write it later.
struct Rendered {
    Node           node;
    const NodeOps* ops = nullptr;
    const void*    aux = nullptr;
    Editor         editor;
};

/**
 * Reads a node. Nothing here fails: an exception of a formatter or of a paged source becomes the node's value, with the
 * kind `opaque`. A node is writable when the watch is, its type has an editor, and what is shown of it is whole and
 * tells it exactly: neither its label nor its value cut, its value shown as it is (a control character is shown as its
 * picture, which an edition would write back in its place), and not a keyed element told by its position.
 */
[[nodiscard]] inline Rendered render(const ChildView& view, std::string path, bool positional, bool writable) {
    const auto& ops = *view.node.ops;
    std::string kind, value;
    std::size_t count   = 0;
    bool        cut     = false;
    bool        altered = false;
    bool        failed  = false;
    try {
        kind = ops.kind(view.node);
        TextSink sink{maxTextBytes + 1};  // one byte more than what is shown tells that it is cut
        ops.value(view.node, sink);
        std::tie(value, cut) = visibleText(sink.text(), maxTextBytes, true);
        altered              = !cut && value != sink.text();
        count                = ops.container ? ops.size(view.node) : 0;
        // The number travels as a double: beyond 2^53 it would no longer be exact, and could not be read back.
        // Compared in 64-bit integers, where 2^53 fits whatever the width of size_t: a 32-bit count never reaches it, and no
        // shift or conversion out of range is compiled there. JavaScript's safe integers stop at 2^53 - 1.
        if (static_cast<std::uint64_t>(count) >= std::uint64_t{1} << 53)
            throw std::length_error("more elements than a page can tell (2^53 - 1)");
    } catch (const std::exception& error) {
        failed = true;
        value  = exceptionValue(error.what());
    } catch (...) {
        failed = true;
        value  = exceptionValue("unknown");
    }
    if (failed) {
        kind  = "opaque";
        count = 0;
        cut   = false;
    }
    unsigned flags = cut ? flag::truncated : 0u;
    Editor   editor;
    if (writable && !failed && !cut && !altered && !view.labelCut && !positional && ops.editor != nullptr) {
        try {
            editor = ops.editor(view.node);
        } catch (...) {
            editor = nullptr;
        }
        if (editor)
            flags |= flag::writable;
    }
    if (!failed && depthReached(path, ops))
        flags |= flag::depthReached;
    return {
        .node   = Node{std::move(path), std::move(kind), view.label, std::move(value), static_cast<double>(count), static_cast<double>(flags)},
        .ops    = &ops,
        .aux    = view.node.aux,
        .editor = std::move(editor)
    };
}

/// The key of the child at `position` when it can name it in a path: none for a source without keys, when key() throws,
/// or when its encoding is longer than 128 bytes (a cut key would be ambiguous).
[[nodiscard]] inline std::optional<std::string> usableKey(const NodeRef& parent, std::size_t position) noexcept {
    if (!parent.ops->keyed)
        return std::nullopt;
    try {
        auto key = parent.ops->key(parent, position);
        if (key && encodeKey(*key).size() <= maxKeyBytes)
            return key;
    } catch (...) {  // NOLINT(bugprone-empty-catch): an element whose key fails is an element without a key
    }
    return std::nullopt;
}

/// What visits each node of a path: (level, node, told by its position under a keyed parent). False stops.
using PathHook = FunctionRef<bool(std::size_t, const ChildView&, bool)>;

/**
 * Follows `segments` from `here`, calling `at` for each node from `here` down, while they resolve. A keyed segment is
 * resolved by find(), then checked: the key at the position found must be the same, else the node is gone. Returns
 * whether the walk went as far as `at` let it (false: a segment no longer resolves). `at` must not throw.
 */
inline bool descend(const ChildView& here, std::span<const Segment> segments, std::size_t level, bool positional, PathHook at) {
    if (!at(level, here, positional) || segments.empty())
        return true;
    const auto& segment = segments.front();
    const auto& ops     = *here.node.ops;
    bool        found   = false;
    try {
        if (!ops.container)
            return false;
        if (segment.letter == 'k') {
            if (!ops.keyed)
                return false;
            const auto position = ops.find(here.node, segment.key);
            if (!position)
                return false;
            const auto key = ops.key(here.node, *position);
            if (!key || *key != segment.key)
                return false;
            ops.visit(here.node, *position, 1, [&](const ChildView& child) {
                ChildView named                       = child;
                std::tie(named.label, named.labelCut) = visibleText(segment.key, maxTextBytes, true);
                found                                 = descend(named, segments.subspan(1), level + 1, positional, at);
                return false;
            });
        } else {
            if (segment.letter != ops.letter || segment.number > std::numeric_limits<std::size_t>::max())
                return false;
            const auto position = ops.alternative ? std::size_t{0} : static_cast<std::size_t>(segment.number);
            if (position >= ops.size(here.node))
                return false;
            ops.visit(here.node, position, 1, [&](const ChildView& child) {
                if (child.rank == segment.number)
                    found = descend(child, segments.subspan(1), level + 1, positional || ops.keyed, at);
                return false;
            });
        }
    } catch (...) {
        return false;  // a source that fails while it is walked no longer shows the node
    }
    return found;
}

/// How an operation reaches the object of its watch, for the time of one call.
struct Access {
    /// Calls its argument once with the root, inside the window the application defines (its lock, its executor); false
    /// when the source did not give its object.
    std::function<bool(FunctionRef<void(const NodeRef&)>)> open;
    std::string                                            label;  // of the root: the watch's name, visible and bounded
    bool                                                   writable = false;
    /// The application's own way to write a value, or nothing: (root object, path, text). Throws to refuse.
    std::function<void(void*, std::string_view, std::string_view)> mutate;
    std::function<void(std::string_view)>                          diagnose;  // ADR-011: what the application should know
    std::atomic<bool>                                              duplicateKeysReported{false};
};

/**
 * Runs `use` with the root inside the access window. A source that fails before giving its object rejects the call, with
 * nothing changed; one that fails after it is only reported, since the operation is done (ADR-027, "Atomicité des refus").
 * `use` must not throw.
 */
template <typename Use>
void enter(Access& access, Use&& use) {
    bool       entered = false;
    const auto failure = [&](std::string_view what) {
        if (!entered)
            throw std::runtime_error("The watch's source failed: " + boundedText(what, maxTextBytes - 32));
        // The operation is done, a write may be applied: a diagnostic that fails does not replace its answer.
        try {
            if (access.diagnose)
                access.diagnose("The source of the watch '" + access.label + "' threw after its visit: " + boundedText(what));
        } catch (...) {  // NOLINT(bugprone-empty-catch): a diagnostic is not worth losing the answer
        }
    };
    try {
        if (!access.open)
            throw refusal(code::unavailable, "The watch has no source");
        static_cast<void>(access.open([&](const NodeRef& root) {
            if (entered)
                return;  // a source calls its visit once: a second call reads nothing
            entered = true;
            use(root);
        }));
    } catch (const BridgeError&) {
        if (!entered)
            throw;
    } catch (const std::exception& error) {
        failure(error.what());
    } catch (...) {
        failure("unknown exception");
    }
    if (!entered)
        throw refusal(code::unavailable, "The watch's source did not give its object");
}

/**
 * @brief The memory of one connection for one watch: the nodes it was sent, under a revision, and the pass of differences
 * in progress. Every call that changes it validates first, without effect, then acts inside the access window and always
 * answers with the resulting revision (ADR-027, section 3).
 */
class Memory {
public:
    struct Entry {
        Node                               node;  // as it was last sent, without the flags that tell an event
        const NodeOps*                     ops = nullptr;
        const void*                        aux = nullptr;
        Editor                             editor;
        std::map<std::size_t, std::string> positions;  // the children loaded: position, path
        std::optional<std::string>         lastKey;    // of the last child served, for a keyed parent
        std::size_t                        lastPosition = 0;
    };

    [[nodiscard]] std::uint64_t revision() const {
        std::scoped_lock lock{mutex};
        return current;
    }
    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock{mutex};
        return nodes.size();
    }
    [[nodiscard]] bool contains(std::string_view path) const {
        std::scoped_lock lock{mutex};
        return nodes.contains(path);
    }
    /// Whether a pass of differences has pages left to serve.
    [[nodiscard]] bool passing() const {
        std::scoped_lock lock{mutex};
        return pass.has_value();
    }

    /// webfront.inspector.children: a page of the children of `path`, its first node being the parent itself.
    Page children(Access& access, std::string_view path, double revisionAsked, double startAsked, double countAsked, const Limits& limits) {
        const auto       segments = segmentsOf(path);
        const auto       revision = wholeNumber(revisionAsked, "revision");
        const auto       start    = wholeNumber(startAsked, "position");
        const auto       count    = (std::min<std::uint64_t>)(wholeNumber(countAsked, "count"), maxPageCount);
        std::scoped_lock lock{mutex};
        if (revision != current)
            return resynchronize();
        const Entry* known = restored(path);
        if (known == nullptr && !path.empty())
            throw refusal(code::unknown, "The node is not in the inspector's memory: unfold its parent first");
        if (known != nullptr && (flagsOf(known->node) & flag::depthReached) != 0)
            throw refusal(code::limit, "The node is too deep to be unfolded");
        if (sizeRestored() >= limits.maxNodes)
            throw refusal(code::limit, "The inspector's memory is full: fold some nodes");
        Page page;
        enter(access, [&](const NodeRef& root) {
            interrupt();
            page = list(access, root, std::string(path), segments, static_cast<std::size_t>(start), static_cast<std::size_t>(count), limits);
        });
        return page;
    }

    /// webfront.inspector.changes: a page of a pass of differences; the cursor 0 starts a pass on the object as it is.
    Page changes(Access& access, double revisionAsked, double cursorAsked, double countAsked, const Limits& limits) {
        const auto       revision = wholeNumber(revisionAsked, "revision");
        const auto       cursor   = wholeNumber(cursorAsked, "cursor");
        const auto       count    = std::clamp<std::uint64_t>(wholeNumber(countAsked, "count"), 1, maxPageCount);
        std::scoped_lock lock{mutex};
        if (cursor != 0) {
            // A later page is read in the set kept by the pass: a revision or a cursor that is not the pass's is a page lost.
            if (!pass || revision != current || cursor != pass->served)
                return resynchronize();
            return serve(static_cast<std::size_t>(count), limits);
        }
        if (revision != current)
            return resynchronize();
        enter(access, [&](const NodeRef& root) {
            interrupt();
            compare(access, root);
        });
        return serve(static_cast<std::size_t>(count), limits);
    }

    /// webfront.inspector.fold: forgets the descendants of `path`. The root stays.
    Page fold(std::string_view path, double revisionAsked) {
        static_cast<void>(segmentsOf(path));
        const auto       revision = wholeNumber(revisionAsked, "revision");
        std::scoped_lock lock{mutex};
        if (revision != current)
            return resynchronize();
        if (restored(path) == nullptr)
            throw refusal(code::unknown, "The node is not in the inspector's memory");
        interrupt();
        auto& entry = nodes.find(path)->second;
        dropDescendants(path, nullptr);
        entry.positions.clear();
        entry.lastKey.reset();
        return {std::vector<Node>{}, 0.0, static_cast<double>(++current), false};
    }

    /// webfront.inspector.reset: empties the memory and gives the revision of the empty one.
    double reset() {
        std::scoped_lock lock{mutex};
        nodes.clear();
        pass.reset();
        return static_cast<double>(++current);
    }

    /**
     * webfront.inspector.write. What can be refused without reading the object rejects (exceptions, nothing changed); then,
     * inside the access window, a pass in progress is interrupted and the target and its ancestors must still show what
     * the memory holds, the target showing `expected`. The answer then always carries the resulting revision, with the
     * code `stale` or `refused` when the value was not written. `applied` tells that the object was changed.
     */
    WriteAnswer
    write(Access& access, double revisionAsked, std::string_view path, std::string_view expected, std::string_view text, const Limits& limits, bool& applied) {
        if (text.size() > limits.maxWriteTextBytes)
            throw refusal(code::tooLarge, "The text is longer than " + std::to_string(limits.maxWriteTextBytes) + " bytes");
        const auto       segments = segmentsOf(path);
        const auto       revision = wholeNumber(revisionAsked, "revision");
        std::scoped_lock lock{mutex};
        if (revision != current)
            throw refusal(code::stale, "The inspector's revision is no longer the memory's");
        const Entry* known = restored(path);
        if (known == nullptr)
            throw refusal(code::unknown, "The node is not in the inspector's memory");
        if (!access.writable || (flagsOf(known->node) & flag::writable) == 0 || !known->editor)
            throw refusal(code::readOnly, "The node is not writable");
        Applier apply;
        try {
            apply = known->editor(text);
        } catch (const std::exception& error) {
            throw refusal(code::parse, boundedText(error.what()));
        }
        WriteAnswer answer;
        enter(access, [&](const NodeRef& root) {
            interrupt();
            std::string code, message;
            bool        reached = false;
            const auto  whole   = descend(rootView(access, root), segments, 0, false, [&](std::size_t level, const ChildView& view, bool positional) {
                const auto at    = prefix(path, level);
                const auto found = nodes.find(at);
                if (found == nodes.end())
                    return false;
                auto&      entry = found->second;
                const auto now   = render(view, std::string(at), positional, access.writable);
                // What the user saw of the node must still be true: its kind, its label, its value, its number of children.
                if (std::get<1>(now.node) != std::get<1>(entry.node) || std::get<2>(now.node) != std::get<2>(entry.node)
                    || std::get<3>(now.node) != std::get<3>(entry.node) || std::get<4>(now.node) != std::get<4>(entry.node))
                    return false;
                if (level != segments.size())
                    return true;
                if (std::get<3>(now.node) != expected || now.ops != entry.ops || now.aux != entry.aux || (flagsOf(now.node) & flag::writable) == 0)
                    return false;
                reached = true;
                // A mutator may change the container being walked: it is called once the walk is over. An assignment writes
                // the node where it is.
                if (access.mutate)
                    return true;
                try {
                    apply(view.node);
                    applied = true;
                } catch (const std::exception& error) {
                    code    = inspect::code::refused;
                    message = boundedText(error.what());
                } catch (...) {
                    code    = inspect::code::refused;
                    message = "The application refused the value";
                }
                return true;
            });
            if (whole && reached && access.mutate) {
                try {
                    access.mutate(root.object, path, text);
                    applied = true;
                } catch (const std::exception& error) {
                    code    = inspect::code::refused;
                    message = boundedText(error.what());
                } catch (...) {
                    code    = inspect::code::refused;
                    message = "The application refused the value";
                }
            }
            // The memory keeps what the inspector was sent: the answer carries no value, and the object may hold another one
            // than the text (a mutator that adjusts it, a member that normalizes it). The next pass, or a reading of the
            // node, tells the value it holds now.
            // A source that fails after the value was assigned (a paged source that throws once it gave the element) does
            // not make the write stale: it was applied.
            if ((!whole || !reached) && !applied)
                code = inspect::code::stale;
            answer = {static_cast<double>(++current), std::move(code), std::move(message)};
        });
        return answer;
    }

    /// The whole value of a node of the memory, formatted inside the access window up to `limit` bytes, and whether it was
    /// cut there. The memory is not changed.
    std::pair<std::string, bool> value(Access& access, std::string_view path, std::size_t limit) {
        const auto       segments = segmentsOf(path);
        std::scoped_lock lock{mutex};
        if (restored(path) == nullptr)
            throw refusal(code::unknown, "The node is not in the inspector's memory");
        std::pair<std::string, bool> text;
        std::string                  failure;
        bool                         reached = false;
        enter(access, [&](const NodeRef& root) {
            static_cast<void>(descend(rootView(access, root), segments, 0, false, [&](std::size_t level, const ChildView& view, bool) {
                if (level != segments.size())
                    return true;
                reached = true;
                try {
                    TextSink sink{limit == std::numeric_limits<std::size_t>::max() ? limit : limit + 1};
                    view.node.ops->value(view.node, sink);
                    text = visibleText(sink.text(), limit, false);
                } catch (const std::exception& error) {
                    failure = "The value could not be formatted: " + boundedText(error.what(), maxTextBytes - 40);
                } catch (...) {
                    failure = "The value could not be formatted";
                }
                return true;
            }));
        });
        if (!reached)
            throw refusal(code::stale, "The node is no longer in the object");
        if (!failure.empty())
            throw std::runtime_error(failure);
        return text;
    }

private:
    /// A change of a pass: what the node shows now, and what the memory held, to give it back if the page is never served.
    struct Change {
        std::string                                path;
        Rendered                                   now;
        bool                                       dropChildren = false;
        Entry                                      before;
        std::vector<std::pair<std::string, Entry>> dropped;
    };
    struct Pass {
        std::vector<Change> changes;
        std::size_t         served = 0;
    };

    [[nodiscard]] static std::vector<Segment> segmentsOf(std::string_view path) {
        auto segments = parsePath(path);
        if (!segments)
            throw refusal(code::unknown, "Ill-formed path");
        return std::move(*segments);
    }

    [[nodiscard]] static ChildView rootView(const Access& access, const NodeRef& root) {
        return {.position = 0, .rank = 0, .node = root, .label = access.label, .labelCut = false};
    }

    /// The path of the ancestor of `path` at `level` (0 is the root).
    [[nodiscard]] static std::string_view prefix(std::string_view path, std::size_t level) noexcept {
        std::size_t end = 0;
        for (std::size_t seen = 0; seen < level; ++seen) {
            const auto next = path.find('/', end + 1);
            if (next == std::string_view::npos)
                return path;
            end = next;
        }
        return path.substr(0, end);
    }

    /// The memory emptied under a new revision, which the answer carries: the inspector reads again from the root.
    Page resynchronize() {
        nodes.clear();
        pass.reset();
        return {std::vector<Node>{}, 0.0, static_cast<double>(++current), true};
    }

    /// What the memory would hold for `path` once a pass in progress is interrupted: exactly what the inspector received.
    [[nodiscard]] const Entry* restored(std::string_view path) const {
        if (pass)
            for (std::size_t index = pass->served; index < pass->changes.size(); ++index) {
                const auto& change = pass->changes[index];
                if (change.path == path)
                    return &change.before;
                for (const auto& [dropped, entry] : change.dropped)
                    if (dropped == path)
                        return &entry;
            }
        const auto found = nodes.find(path);
        return found == nodes.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::size_t sizeRestored() const {
        auto size = nodes.size();
        if (pass)
            for (std::size_t index = pass->served; index < pass->changes.size(); ++index)
                size += pass->changes[index].dropped.size();
        return size;
    }

    /// Ends a pass in progress: the entries of the pages not served get back what they held before the pass, so that the
    /// memory tells exactly what the inspector received. The next pass finds those differences again.
    void interrupt() {
        if (!pass)
            return;
        for (std::size_t index = pass->changes.size(); index > pass->served; --index) {
            auto& change       = pass->changes[index - 1];
            nodes[change.path] = std::move(change.before);
            for (auto& [path, entry] : change.dropped)
                nodes[path] = std::move(entry);
        }
        pass.reset();
    }

    /// Removes the descendants of `path`, keeping them in `kept` when given.
    void dropDescendants(std::string_view path, std::vector<std::pair<std::string, Entry>>* kept) {
        // Every descendant's path starts with the parent's and a '/': they follow it in the order of the paths.
        std::string from{path};
        from   += '/';
        auto it = nodes.lower_bound(from);
        while (it != nodes.end() && std::string_view{it->first}.starts_with(from)) {
            if (kept != nullptr)
                kept->emplace_back(it->first, std::move(it->second));
            it = nodes.erase(it);
        }
    }

    static void store(Entry& entry, Rendered now) {
        if (entry.ops != now.ops || entry.aux != now.aux) {
            entry.positions.clear();
            entry.lastKey.reset();
        }
        entry.node   = std::move(now.node);
        entry.ops    = now.ops;
        entry.aux    = now.aux;
        entry.editor = std::move(now.editor);
    }

    /// The page of `children`, inside the access window.
    Page list(Access&                     access,
              const NodeRef&              root,
              const std::string&          path,
              const std::vector<Segment>& segments,
              std::size_t                 start,
              std::size_t                 count,
              const Limits&               limits) {
        std::vector<Node> served;
        std::size_t       next    = 0;
        bool              reached = false;
        static_cast<void>(descend(rootView(access, root), segments, 0, false, [&](std::size_t level, const ChildView& view, bool positional) {
            if (level != segments.size())
                return true;
            reached = true;
            next    = listChildren(access, view, path, positional, start, count, limits, served);
            return true;
        }));
        if (!reached) {
            // The node is gone from the object: it leaves the memory with its descendants, and the inspector is told.
            Node gone{path, "opaque", "", "", 0.0, static_cast<double>(flag::removed)};
            if (const auto found = nodes.find(path); found != nodes.end()) {
                gone              = found->second.node;
                std::get<4>(gone) = 0.0;
                std::get<5>(gone) = static_cast<double>(flag::removed);
                dropDescendants(path, nullptr);
                nodes.erase(found);
            }
            served.push_back(std::move(gone));
        }
        return {std::move(served), static_cast<double>(next), static_cast<double>(++current), false};
    }

    /// Serves the parent then its children from `start`; returns the next position, 0 at the end.
    std::size_t listChildren(Access&            access,
                             const ChildView&   view,
                             const std::string& path,
                             bool               positional,
                             std::size_t        start,
                             std::size_t        count,
                             const Limits&      limits,
                             std::vector<Node>& served) {
        auto        parent = render(view, path, positional, access.writable);
        const auto& ops    = *view.node.ops;
        const auto  total  = childrenOf(parent.node);
        auto&       entry  = nodes[path];
        if (start > 0) {
            // A later page of a list is read on the state of the previous ones, or the list starts again (rule 2).
            bool consistent = entry.ops == parent.ops && total == childrenOf(entry.node);
            if (consistent && entry.lastKey) {
                try {
                    const auto position = ops.find(view.node, *entry.lastKey);
                    consistent          = position && *position == entry.lastPosition;
                } catch (...) {
                    consistent = false;
                }
            }
            // With keys, every element served must still be where it was: one replaced before the last one served keeps
            // the number of children and the place of that last one (one call of key() per position the memory holds).
            if (consistent && ops.keyed)
                for (const auto& [position, loaded] : entry.positions) {
                    const std::string_view segment = std::string_view{loaded}.substr(path.size() + 1);
                    if (!segment.starts_with('k'))
                        continue;
                    const auto key = usableKey(view.node, position);
                    if (!key || 'k' + encodeKey(*key) != segment) {
                        consistent = false;
                        break;
                    }
                }
            if (!consistent) {
                dropDescendants(path, nullptr);
                entry.positions.clear();
                entry.lastKey.reset();
                store(entry, parent);
                std::get<5>(parent.node) = static_cast<double>(flagsOf(parent.node) | flag::childrenChanged);
                served.push_back(std::move(parent.node));
                return 0;
            }
        }
        store(entry, parent);
        // The positions that no longer exist leave the memory with what was under them.
        for (auto it = entry.positions.lower_bound(total); it != entry.positions.end(); it = entry.positions.erase(it))
            forget(it->second);
        const auto budget = limits.responseBudget;
        auto       used   = pageOverheadBytes + nodeBytes(parent.node);
        served.push_back(parent.node);
        if (!ops.container || (flagsOf(parent.node) & flag::depthReached) != 0 || start >= total || count == 0)
            return 0;

        struct Pending {
            std::size_t                position;
            Rendered                   node;
            std::optional<std::string> key;
        };
        std::vector<Pending> pending;
        std::size_t          fresh = 0;      // the entries the page adds to the memory
        bool                 moved = false;  // a key of this page was served at another position
        const auto           each  = [&](const ChildView& child) {
            ChildView  shown = child;
            const auto key   = usableKey(view.node, child.position);
            if (key)
                std::tie(shown.label, shown.labelCut) = visibleText(*key, maxTextBytes, true);
            const auto told     = numberSegment(ops.letter, child.rank);
            const auto childAt  = childPath(path, key ? 'k' + encodeKey(*key) : told);
            auto       rendered = render(shown, childAt, positional || (ops.keyed && !key), access.writable);
            // A keyed child may yet be told by its position (a key that another child of the page has too): the page keeps
            // room for the longer of its two paths.
            const auto longer = (std::max)(childAt.size(), path.size() + 1 + told.size());
            const auto bytes  = nodeBytes(rendered.node) + stringBytes(longer) - stringBytes(childAt.size());
            const bool added  = !nodes.contains(childAt);
            if (start > 0 && key) {
                // A key served at another position of this list: the source moved its elements between two pages.
                for (const auto& [position, loaded] : entry.positions)
                    if (loaded == childAt && position != child.position) {
                        moved = true;
                        return false;
                    }
            }
            if (used + bytes > budget || (added && nodes.size() + fresh >= limits.maxNodes))
                return false;
            used  += bytes;
            fresh += added ? 1 : 0;
            pending.push_back({.position = child.position, .node = std::move(rendered), .key = key});
            return pending.size() < count;
        };
        bool failed = false;
        try {
            ops.visit(view.node, start, count, each);
        } catch (...) {
            // A paged source that fails while it gives its elements: the page serves those it gave, and ends there.
            failed = true;
        }
        if (moved) {
            // As when the number of children changed (rule 2): the list starts again from its first position.
            dropDescendants(path, nullptr);
            entry.positions.clear();
            entry.lastKey.reset();
            std::get<5>(served.front()) = static_cast<double>(flagsOf(served.front()) | flag::childrenChanged);
            return 0;
        }

        // Two elements of a page with the same key break the key's contract: both are told by their position, and neither
        // is writable (ADR-027, section 5).
        std::map<std::string, std::size_t, std::less<>> seen;
        for (const auto& child : pending)
            if (child.key)
                ++seen[*child.key];
        for (auto& child : pending) {
            if (!child.key || seen.find(*child.key)->second < 2)
                continue;
            auto& node        = child.node.node;
            std::get<0>(node) = childPath(path, numberSegment(ops.letter, child.position));
            auto flags        = flagsOf(node) & ~(flag::writable | flag::depthReached);
            if (std::get<1>(node) != "opaque" && depthReached(std::get<0>(node), *child.node.ops))
                flags |= flag::depthReached;
            std::get<5>(node) = static_cast<double>(flags);
            child.node.editor = nullptr;
            child.key.reset();
            if (!access.duplicateKeysReported.exchange(true) && access.diagnose)
                access.diagnose("The watch '" + access.label + "' has elements with the same key in one page: they are shown by their position");
        }

        std::vector<std::string> replaced;
        for (auto& child : pending) {
            const std::string childAt = std::get<0>(child.node.node);  // a copy: the node is moved into the memory below
            if (const auto loaded = entry.positions.find(child.position); loaded != entry.positions.end() && loaded->second != childAt)
                replaced.push_back(loaded->second);
            entry.positions[child.position] = childAt;
            served.push_back(child.node.node);
            store(nodes[childAt], std::move(child.node));
        }
        if (!replaced.empty()) {
            std::set<std::string_view> loaded;
            for (const auto& [position, childAt] : entry.positions)
                loaded.insert(childAt);
            for (const auto& old : replaced)
                if (!loaded.contains(old))
                    forget(old);
        }
        if (!pending.empty()) {
            entry.lastKey      = pending.back().key;
            entry.lastPosition = pending.back().position;
        }
        const auto after = start + pending.size();
        return after < total && !failed ? after : 0;
    }

    /// Removes a node and its descendants.
    void forget(const std::string& path) {
        dropDescendants(path, nullptr);
        nodes.erase(path);
    }

    /// Starts a pass: compares the memory with the object in one read, keeps the differences with what the memory held,
    /// and gives the memory the new values.
    void compare(Access& access, const NodeRef& root) {
        Pass started;
        if (const auto found = nodes.find(std::string_view{}); found != nodes.end())
            walk(access, rootView(access, root), std::string{}, false, found->second, started.changes);
        for (auto& change : started.changes) {
            auto& entry   = nodes.find(change.path)->second;
            change.before = entry;
            if (change.dropChildren) {
                dropDescendants(change.path, &change.dropped);
                entry.positions.clear();
                entry.lastKey.reset();
            }
            Rendered kept          = change.now;
            std::get<5>(kept.node) = static_cast<double>(flagsOf(kept.node) & ~flag::childrenChanged);
            store(entry, std::move(kept));
        }
        ++current;
        if (!started.changes.empty())
            pass = std::move(started);
    }

    /// Compares a node of the memory and, when it is unfolded, the structure of its children then each child loaded.
    void walk(Access& access, const ChildView& view, const std::string& path, bool positional, const Entry& entry, std::vector<Change>& out) {
        auto       now  = render(view, path, positional, access.writable);
        const auto mark = out.size();
        if (now.node != entry.node || now.ops != entry.ops || now.aux != entry.aux)
            out.push_back({.path = path, .now = now, .dropChildren = false, .before = {}, .dropped = {}});
        if (entry.positions.empty())
            return;
        const auto& ops       = *view.node.ops;
        bool        structure = now.ops != entry.ops || childrenOf(now.node) != childrenOf(entry.node);
        // A container that reaches a position at once is read by runs of loaded positions. Any other is walked once, from
        // its first loaded position to its last (rule 3): each visit starts again from its beginning.
        for (auto run = entry.positions.begin(); run != entry.positions.end() && !structure;) {
            auto        last   = run;
            std::size_t length = 1;  // the loaded positions of the run
            for (auto following = std::next(run); following != entry.positions.end() && (!ops.direct || following->first == last->first + 1); ++following) {
                last = following;
                ++length;
            }
            std::size_t visited = 0;
            try {
                ops.visit(view.node, run->first, last->first - run->first + 1, [&](const ChildView& child) {
                    const auto loaded = entry.positions.find(child.position);
                    if (loaded == entry.positions.end())
                        return true;
                    ++visited;
                    const std::string_view recorded = std::string_view{loaded->second}.substr(path.size() + 1);
                    ChildView              shown    = child;
                    bool                   byPlace  = positional;  // under a parent told by its position, so is every child
                    // A keyed child is labelled by its key, also when it is told by its position (a key that repeats).
                    const auto key = usableKey(view.node, child.position);
                    if (key)
                        std::tie(shown.label, shown.labelCut) = visibleText(*key, maxTextBytes, true);
                    if (ops.keyed && recorded.starts_with('k')) {
                        if (!key || 'k' + encodeKey(*key) != recorded) {
                            structure = true;
                            return false;
                        }
                    } else {
                        byPlace = positional || ops.keyed;
                        if (numberSegment(ops.letter, ops.keyed ? child.position : child.rank) != recorded) {
                            structure = true;
                            return false;
                        }
                    }
                    if (const auto found = nodes.find(loaded->second); found != nodes.end())
                        walk(access, shown, loaded->second, byPlace, found->second, out);
                    return true;
                });
            } catch (...) {
                structure = true;
            }
            if (visited != length)
                structure = true;
            run = std::next(last);
        }
        if (structure) {
            // The children are no longer those the memory holds: they leave it, and the inspector reads the list again.
            out.resize(mark);
            std::get<5>(now.node) = static_cast<double>(flagsOf(now.node) | flag::childrenChanged);
            out.push_back({.path = path, .now = std::move(now), .dropChildren = true, .before = {}, .dropped = {}});
        }
    }

    /// The next page of the pass, read in the set it keeps.
    Page serve(std::size_t count, const Limits& limits) {
        std::vector<Node> served;
        if (!pass)
            return {std::move(served), 0.0, static_cast<double>(current), false};
        auto used = pageOverheadBytes;
        while (pass->served < pass->changes.size() && served.size() < count) {
            const auto& node  = pass->changes[pass->served].now.node;
            const auto  bytes = nodeBytes(node);
            if (used + bytes > limits.responseBudget && !served.empty())
                break;
            used += bytes;
            served.push_back(node);
            ++pass->served;
        }
        const auto next = pass->served < pass->changes.size() ? pass->served : 0;
        if (next == 0)
            pass.reset();
        return {std::move(served), static_cast<double>(next), static_cast<double>(current), false};
    }

    mutable std::mutex                        mutex;
    std::uint64_t                             current = 1;
    std::map<std::string, Entry, std::less<>> nodes;
    std::optional<Pass>                       pass;
};

}  // namespace webfront::inspect::detail
