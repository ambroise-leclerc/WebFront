// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: The passes of differences of the watches (ADR-027, section 3): pages of a coherent set, interruption, stale revisions, resynchronization.
// Thread: worker-safe | Alloc: unrestricted
// LLM: unit tests of Memory::changes and of what interrupts a pass; trees are in WatchTreeTests.cpp, writes in WatchWriteTests.cpp

#include "WatchHarness.hpp"

#include <format>
#include <optional>
#include <variant>

using namespace webfront::bdd;
using namespace webfront;

namespace watch_pass {
using namespace watch_fixture;

/// Its formatter throws while `failing` is set.
struct Fragile {
    int  value   = 0;
    bool failing = false;
};

/// A keyed paged source of numbers.
struct Rows {
    std::vector<std::pair<std::string, int>> rows;
    bool                                     keyFails = false;
    [[nodiscard]] std::size_t                size() const {
        return rows.size();
    }
    void visit(std::size_t first, std::size_t count, auto&& each) {
        for (std::size_t index = first; index < rows.size() && index - first < count; ++index)
            each(rows[index].second);
    }
    [[nodiscard]] std::string key(std::size_t position) const {
        if (keyFails)
            throw std::runtime_error("no key");
        return rows.at(position).first;
    }
    [[nodiscard]] std::optional<std::size_t> find(std::string_view wanted) const {
        for (std::size_t index = 0; index < rows.size(); ++index)
            if (rows[index].first == wanted)
                return index;
        return std::nullopt;
    }
};

/// Reads the whole list of the root, 1 000 children a page; returns the last page.
Page loadAll(Bench& bench, std::uint64_t id) {
    auto page = bench.open(id, 1000);
    while (nextOf(page) != 0)
        page = bench.children(id, "", revisionOf(page), nextOf(page), 1000);
    return page;
}

/// Reads a pass to its end from its first page; returns the paths of all its nodes.
std::vector<std::string> finish(Bench& bench, std::uint64_t id, Page page, double count = 1000) {
    auto paths = pathsOf(page);
    while (nextOf(page) != 0) {
        page = bench.changes(id, revisionOf(page), nextOf(page), count);
        webfront::bdd::require(!resyncOf(page));
        for (const auto& path : pathsOf(page))
            paths.push_back(path);
    }
    return paths;
}
}  // namespace watch_pass

template <>
struct std::formatter<watch_pass::Fragile> {
    constexpr auto parse(std::format_parse_context& context) {
        return context.begin();
    }
    auto format(const watch_pass::Fragile& fragile, std::format_context& context) const {
        if (fragile.failing)
            throw std::runtime_error("broken");
        return std::format_to(context.out(), "fragile {}", fragile.value);
    }
};

namespace watch_pass {

const Scenario watchChanges{"A pass of differences tells what changed since the memory was written (ADR-027, section 3, \"Différences paginées\")",
                            {"inspector"},
                            [] {
                                Bench                         bench;
                                std::vector<std::vector<int>> grid{
                                    {1, 2, 3},
                                    {4, 5}
                                };
                                const auto id   = bench.watch("grid", visiting(grid));
                                auto       page = bench.open(id);
                                page            = bench.children(id, "/#0", revisionOf(page));
                                then("nothing changed: an empty page, under a new revision", [&] {
                                    const auto pass = bench.changes(id, revisionOf(page));
                                    require(nodesOf(pass).empty() && !resyncOf(pass));
                                    requireEqual(nextOf(pass), 0.0);
                                    require(revisionOf(pass) != revisionOf(page));
                                });
                                then("a value that changed is told with its new value, and only once", [&] {
                                    grid[0][1]      = 20;
                                    const auto pass = bench.changes(id, revisionOf(page));
                                    requireEqual(pathsOf(pass), std::vector<std::string>{"/#0/#1"});
                                    requireEqual(valueOf(nodesOf(pass).front()), std::string{"20"});
                                    require(nodesOf(bench.changes(id, revisionOf(pass))).empty());
                                });
                                then("what is not unfolded is not read: a change under a folded node tells nothing", [&] {
                                    grid[1][0] = 40;
                                    require(nodesOf(bench.changes(id, revisionOf(page))).empty());
                                });
                                then("a parent whose number of children changed is told \"children changed\", and its children leave the memory", [&] {
                                    grid[0].push_back(9);
                                    const auto pass = bench.changes(id, revisionOf(page));
                                    requireEqual(pathsOf(pass), std::vector<std::string>{"/#0"});
                                    requireEqual(flagsOf(nodesOf(pass).front()), flag::childrenChanged);
                                    requireEqual(countOf(nodesOf(pass).front()), std::size_t{4});
                                    require(!bench.memory(id)->contains("/#0/#0"));
                                    // The inspector reads the list again; the next pass has nothing left to tell.
                                    const auto again = bench.children(id, "/#0", revisionOf(pass));
                                    requireEqual(nodesOf(again).size(), std::size_t{5});
                                    require(nodesOf(bench.changes(id, revisionOf(again))).empty());
                                });
                                then("an optional that empties and a variant that changes alternative are told the same way", [&] {
                                    std::tuple<std::optional<int>, std::variant<int, std::string>> wrapped{5, 6};
                                    const auto                                                     other  = bench.watch("wrapped", visiting(wrapped));
                                    auto                                                           opened = bench.open(other);
                                    opened                                                                = bench.children(other, "/#0", revisionOf(opened));
                                    opened                                                                = bench.children(other, "/#1", revisionOf(opened));
                                    requireEqual(pathOf(nodesOf(opened).back()), std::string{"/#1/#0"});
                                    std::get<0>(wrapped).reset();
                                    std::get<1>(wrapped) = std::string{"now text"};
                                    const auto pass      = bench.changes(other, revisionOf(opened));
                                    requireEqual(pathsOf(pass), std::vector<std::string>{"/#0", "/#1"});
                                    checkEqual(flagsOf(nodesOf(pass)[0]), flag::childrenChanged);
                                    checkEqual(valueOf(nodesOf(pass)[0]), std::string{"nullopt"});
                                    checkEqual(flagsOf(nodesOf(pass)[1]), flag::childrenChanged);
                                    checkEqual(valueOf(nodesOf(pass)[1]), std::string{"alternative 1"});
                                    requireEqual(bench.memory(other)->size(), std::size_t{3});
                                });
                                then("a pass of several pages is read in the set it kept, whatever the object becomes meanwhile", [&] {
                                    std::vector<int> values(1000, 0);
                                    const auto       other  = bench.watch("values", visiting(values));
                                    auto             loaded = loadAll(bench, other);
                                    for (auto& value : values)
                                        value = 1;
                                    auto pass = bench.changes(other, revisionOf(loaded), 0, 100);
                                    requireEqual(nodesOf(pass).size(), std::size_t{100});
                                    requireEqual(nextOf(pass), 100.0);
                                    const auto revision = revisionOf(pass);
                                    for (auto& value : values)
                                        value = 2;  // between two pages
                                    std::size_t told = 100;
                                    while (nextOf(pass) != 0) {
                                        pass = bench.changes(other, revision, nextOf(pass), 100);
                                        require(!resyncOf(pass));
                                        requireEqual(revisionOf(pass), revision);
                                        for (const auto& node : nodesOf(pass))
                                            requireEqual(valueOf(node), std::string{"1"});
                                        told += nodesOf(pass).size();
                                    }
                                    requireEqual(told, std::size_t{1000});
                                    // The inspector holds the revision of the pass, and the next one finds what changed since.
                                    const auto next = bench.changes(other, revision, 0, 1000);
                                    requireEqual(valueOf(nodesOf(next).front()), std::string{"2"});
                                    requireEqual(finish(bench, other, next).size(), std::size_t{1000});
                                });
                            }};

const Scenario watchPassInterrupted{
    "A pass of thousands of pages is interrupted at any time, without a resynchronization (ADR-027, section 3, \"Interruption\")",
    {"inspector"},
    [] {
        Bench bench;
        bench.limits.responseBudget = inspect::minPageBytes;
        std::vector<int> values(3000);
        for (std::size_t index = 0; index < values.size(); ++index)
            values[index] = static_cast<int>(index);
        const auto id     = bench.watch("values", visiting(values));
        auto       loaded = loadAll(bench, id);
        requireEqual(bench.memory(id)->size(), std::size_t{3001});
        const auto changeUpTo = [&](std::size_t end) {
            for (std::size_t index = 0; index < end; ++index)
                values[index] += 10'000;
        };
        // A page of one node: the pass would take 3 000 round trips. Five are made before it is interrupted.
        const auto startPass = [&] {
            auto pass = bench.changes(id, revisionOf(loaded), 0, 1);
            requireEqual(pathsOf(pass), std::vector<std::string>{"/#0"});
            for (int served = 1; served < 5; ++served) {
                pass = bench.changes(id, revisionOf(pass), nextOf(pass), 1);
                requireEqual(nodesOf(pass).size(), std::size_t{1});
            }
            requireEqual(nextOf(pass), 5.0);
            require(bench.memory(id)->passing());
            return pass;
        };
        then("by a new pass: the differences not served are found again", [&] {
            changeUpTo(3000);
            const auto pass    = startPass();
            const auto again   = bench.changes(id, revisionOf(pass), 0, 1000);
            const auto missing = finish(bench, id, again);
            require(!resyncOf(again));
            requireEqual(missing.size(), std::size_t{2995});
            requireEqual(missing.front(), std::string{"/#5"});
        });
        then("by children: the page read is fresh, the rest is restored and found by the next pass", [&] {
            changeUpTo(3000);
            const auto pass = startPass();
            const auto page = bench.children(id, "", revisionOf(pass), 0, 10);
            require(!resyncOf(page));
            requireEqual(valueOf(nodesOf(page)[10]), std::string{"10009"});
            require(!bench.memory(id)->passing());
            requireEqual(bench.memory(id)->size(), std::size_t{3001});
            const auto missing = finish(bench, id, bench.changes(id, revisionOf(page), 0, 1000));
            requireEqual(missing.size(), std::size_t{2990});
            requireEqual(missing.front(), std::string{"/#10"});
        });
        then("by fold: the fold is done on the restored memory", [&] {
            changeUpTo(3000);
            const auto pass   = startPass();
            const auto folded = bench.fold(id, "/#7", revisionOf(pass));
            require(!resyncOf(folded));
            requireEqual(finish(bench, id, bench.changes(id, revisionOf(folded), 0, 1000)).size(), std::size_t{2995});
        });
        then("by a write whose target still shows what was sent: the value is written", [&] {
            changeUpTo(2000);
            const auto pass   = startPass();
            const auto answer = bench.write(id, revisionOf(pass), "/#2999", "2999", "7");
            requireEqual(std::get<1>(answer), std::string{});
            requireEqual(values[2999], 7);
            require(std::get<0>(answer) != revisionOf(pass));
            const auto missing = finish(bench, id, bench.changes(id, std::get<0>(answer), 0, 1000));
            requireEqual(missing.size(), std::size_t{1995});  // the write is in the memory: it is not a difference
        });
        then("by a write whose target is in a page not served: it is compared with what the user sees, and refused as stale", [&] {
            changeUpTo(3000);
            const auto pass   = startPass();
            const auto answer = bench.write(id, revisionOf(pass), "/#2999", "2999", "7");
            requireEqual(std::get<1>(answer), std::string{code::stale});
            requireEqual(values[2999], 12'999);
            require(std::get<0>(answer) != revisionOf(pass));
            // The refusal carries the revision: the next call needs no resynchronization, and loses no difference.
            const auto again = bench.changes(id, std::get<0>(answer), 0, 1000);
            require(!resyncOf(again));
            requireEqual(finish(bench, id, again).size(), std::size_t{2995});
        });
        then("by a write that the mutator refuses: the answer is resolved with the code and the new revision", [&] {
            std::vector<int> guarded(100, 1);
            const auto       other  = bench.watch("guarded", visiting(guarded), [](std::vector<int>&, std::string_view, std::string_view) {
                throw std::runtime_error("not today");
            });
            auto             opened = loadAll(bench, other);
            for (std::size_t index = 0; index < 50; ++index)
                guarded[index] = 2;
            const auto pass   = bench.changes(other, revisionOf(opened), 0, 1);
            const auto answer = bench.write(other, revisionOf(pass), "/#99", "1", "5");
            requireEqual(std::get<1>(answer), std::string{code::refused});
            requireEqual(std::get<2>(answer), std::string{"not today"});
            requireEqual(guarded[99], 1);
            const auto again = bench.changes(other, std::get<0>(answer), 0, 1000);
            require(!resyncOf(again));
            requireEqual(nodesOf(again).size(), std::size_t{49});
        });
        then("a refusal of the validation rejects without interrupting the pass", [&] {
            changeUpTo(3000);
            const auto pass   = startPass();
            const auto memory = bench.memory(id);
            checkEqual(codeOf([&] {
                           return bench.write(id, revisionOf(pass), "/#2999", "2999", "not a number");
                       }),
                       std::string{code::parse});
            checkEqual(codeOf([&] {
                           return bench.write(id, revisionOf(pass), "", "", "1");
                       }),
                       std::string{code::readOnly});
            checkEqual(codeOf([&] {
                           return bench.write(id, revisionOf(pass), "/#2999", "2999", std::string(bench.limits.maxWriteTextBytes + 1, '1'));
                       }),
                       std::string{code::tooLarge});
            checkEqual(codeOf([&] {
                           return bench.write(id, revisionOf(loaded), "/#2999", "2999", "1");
                       }),
                       std::string{code::stale});
            require(memory->passing());
            requireEqual(static_cast<double>(memory->revision()), revisionOf(pass));
            requireEqual(values[2999], 12'999);
            // The pass goes on where it was.
            const auto next = bench.changes(id, revisionOf(pass), nextOf(pass), 1);
            require(!resyncOf(next));
            requireEqual(pathsOf(next), std::vector<std::string>{"/#5"});
        });
    }};

const Scenario watchStaleRevisions{"A revision that is not the memory's has one contract, checked before any effect (ADR-027, section 3, \"Révision périmée\")",
                                   {"inspector"},
                                   [] {
                                       Bench            bench;
                                       std::vector<int> values(50, 1);
                                       const auto       id     = bench.watch("values", visiting(values));
                                       const auto       opened = bench.open(id);
                                       const auto       memory = bench.memory(id);
                                       for (auto& value : values)
                                           value = 2;
                                       const auto pass = bench.changes(id, revisionOf(opened), 0, 10);
                                       require(memory->passing());
                                       const auto resynchronized = [&](const Page& page) {
                                           require(resyncOf(page) && nodesOf(page).empty());
                                           requireEqual(nextOf(page), 0.0);
                                           requireEqual(memory->size(), std::size_t{0});
                                           require(!memory->passing());
                                           requireEqual(revisionOf(page), static_cast<double>(memory->revision()));
                                           // The revision answered opens the root again.
                                           requireEqual(nodesOf(bench.children(id, "", revisionOf(page))).size(), std::size_t{51});
                                       };
                                       then("children with an older revision during a pass: resynchronization, the pass released", [&] {
                                           resynchronized(bench.children(id, "", revisionOf(opened)));
                                       });
                                       then("changes with an older revision during a pass: the same", [&] {
                                           resynchronized(bench.changes(id, revisionOf(opened)));
                                       });
                                       then("fold with an older revision during a pass: the same, and nothing is folded", [&] {
                                           resynchronized(bench.fold(id, "", revisionOf(opened)));
                                       });
                                       then("a cursor that is not the pass's, or a page asked twice, is a page lost", [&] {
                                           resynchronized(bench.changes(id, revisionOf(pass), 7, 10));
                                       });
                                       then("a page of a pass that is over is a page lost", [&] {
                                           auto last = pass;
                                           while (nextOf(last) != 0)
                                               last = bench.changes(id, revisionOf(last), nextOf(last), 10);
                                           require(!memory->passing());
                                           resynchronized(bench.changes(id, revisionOf(last), 40, 10));
                                       });
                                       then("write with an older revision during a pass is refused with no effect, then the next call resynchronizes", [&] {
                                           const auto size = memory->size();
                                           requireEqual(codeOf([&] {
                                                            return bench.write(id, revisionOf(opened), "/#0", "1", "9");
                                                        }),
                                                        std::string{code::stale});
                                           require(memory->passing());
                                           requireEqual(memory->size(), size);
                                           requireEqual(static_cast<double>(memory->revision()), revisionOf(pass));
                                           requireEqual(values[0], 2);
                                           resynchronized(bench.children(id, "", revisionOf(opened)));
                                       });
                                       then("two calls that change the memory sent without waiting: the second is stale", [&] {
                                           const auto first = bench.fold(id, "/#3", revisionOf(pass));
                                           require(!resyncOf(first));
                                           resynchronized(bench.fold(id, "/#4", revisionOf(pass)));
                                       });
                                       then("a removal told only in a page that was lost is neither shown nor kept after the resynchronization", [&] {
                                           values.resize(40);
                                           const auto shrunk = bench.changes(id, revisionOf(pass));  // tells the root's children changed: this page is "lost"
                                           requireEqual(flagsOf(nodesOf(shrunk).front()), flag::childrenChanged);
                                           const auto lost = bench.changes(id, revisionOf(pass));
                                           require(resyncOf(lost));
                                           const auto page = bench.children(id, "", revisionOf(lost));
                                           requireEqual(nodesOf(page).size(), std::size_t{41});
                                           require(!hasNode(page, "/#45"));
                                           requireEqual(memory->size(), std::size_t{41});
                                       });
                                       then("reset empties the memory and releases the pass", [&] {
                                           const auto revision = bench.reset(id);
                                           require(!memory->passing());
                                           requireEqual(memory->size(), std::size_t{0});
                                           requireEqual(nodesOf(bench.children(id, "", revision)).size(), std::size_t{51});
                                       });
                                       then("numbers that are not whole numbers are refused", [&] {
                                           for (const double bad : {-1.0, 0.5, 1e300})
                                               checkEqual(codeOf([&] {
                                                              return bench.changes(id, bad);
                                                          }),
                                                          std::string{"exception"});
                                           require(memory->passing());
                                       });
                                   }};

const Scenario watchPassAtomicity{
    "A call that changed the memory always answers; one that is rejected changed nothing (ADR-027, section 3, \"Atomicité des refus\")",
    {"inspector"},
    [] {
        Bench bench;
        then("children is refused before it interrupts, when the memory restored would be full", [&] {
            bench.limits.maxNodes = 12;
            std::vector<std::vector<int>> grid{
                {1, 2, 3, 4},
                {5, 6, 7, 8, 9}
            };
            const auto id     = bench.watch("grid", visiting(grid));
            auto       page   = bench.open(id);
            page              = bench.children(id, "/#0", revisionOf(page));
            page              = bench.children(id, "/#1", revisionOf(page));
            const auto memory = bench.memory(id);
            requireEqual(memory->size(), std::size_t{12});
            grid[0][0] = 10;
            grid[1].pop_back();
            const auto pass = bench.changes(id, revisionOf(page), 0, 1);
            requireEqual(pathsOf(pass), std::vector<std::string>{"/#0/#0"});
            requireEqual(memory->size(), std::size_t{7});  // the children of /#1 left the memory, in a page not served yet
            requireEqual(codeOf([&] {
                             return bench.children(id, "/#0", revisionOf(pass));
                         }),
                         std::string{code::limit});
            require(memory->passing());
            requireEqual(static_cast<double>(memory->revision()), revisionOf(pass));
            const auto rest = bench.changes(id, revisionOf(pass), nextOf(pass), 1);
            requireEqual(pathsOf(rest), std::vector<std::string>{"/#1"});
            // Once that page is served the memory has room, and the list is read again.
            requireEqual(nodesOf(bench.children(id, "/#1", revisionOf(rest))).size(), std::size_t{5});
        });
        then("after an interruption, a formatter's exception is the value of its node: the page is served", [&] {
            std::vector<Fragile> fragiles(20);
            const auto           id   = bench.watch("fragiles", visiting(fragiles));
            auto                 page = bench.open(id);
            for (auto& fragile : fragiles)
                fragile.value = 1;
            const auto pass     = bench.changes(id, revisionOf(page), 0, 2);
            fragiles[5].failing = true;
            page                = bench.children(id, "", revisionOf(pass));
            require(!resyncOf(page));
            requireEqual(nodesOf(page).size(), std::size_t{21});
            requireEqual(kindOf(nodesOf(page)[6]), std::string{"opaque"});
            require(valueOf(nodesOf(page)[6]).contains("broken"));
            requireEqual(valueOf(nodesOf(page)[7]), std::string{"fragile 1"});
            // And a pass tells it like any other change.
            fragiles[5].failing = false;
            requireEqual(pathsOf(bench.changes(id, revisionOf(page))), std::vector<std::string>{"/#5"});
        });
        then("after an interruption, an exception of key() makes its elements elements without a key", [&] {
            Rows rows{
                .rows     = {{"a", 1}, {"b", 2}, {"c", 3}},
                .keyFails = false
            };
            const auto id   = bench.watch("rows", visiting(rows));
            auto       page = bench.open(id);
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/ka", "/kb", "/kc"});
            rows.rows[0].second = 10;
            rows.rows[1].second = 20;
            const auto pass     = bench.changes(id, revisionOf(page), 0, 1);
            rows.keyFails       = true;
            page                = bench.children(id, "", revisionOf(pass));
            require(!resyncOf(page));
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/#0", "/#1", "/#2"});
            for (std::size_t index = 1; index < 4; ++index)
                checkEqual(flagsOf(nodesOf(page)[index]), 0u);
            // A pass with keys that fail tells the parent's children changed, never an exception.
            rows.keyFails      = false;
            page               = bench.children(id, "", revisionOf(page));
            rows.keyFails      = true;
            const auto failing = bench.changes(id, revisionOf(page));
            requireEqual(pathsOf(failing), std::vector<std::string>{""});
            requireEqual(flagsOf(nodesOf(failing).front()), flag::childrenChanged);
        });
        then("a pass sees the keys of an unfolded parent reordered", [&] {
            Rows rows{
                .rows     = {{"a", 1}, {"b", 2}, {"c", 3}},
                .keyFails = false
            };
            const auto id   = bench.watch("rows", visiting(rows));
            auto       page = bench.open(id);
            std::swap(rows.rows[0], rows.rows[2]);
            const auto pass = bench.changes(id, revisionOf(page));
            requireEqual(pathsOf(pass), std::vector<std::string>{""});
            requireEqual(flagsOf(nodesOf(pass).front()), flag::childrenChanged);
            requireEqual(bench.memory(id)->size(), std::size_t{1});
            // The positions are read again.
            page = bench.children(id, "", revisionOf(pass));
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/kc", "/kb", "/ka"});
            require(nodesOf(bench.changes(id, revisionOf(page))).empty());
        });
    }};
}  // namespace watch_pass
