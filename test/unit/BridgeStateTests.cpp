// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: The relay of the state of a page's bridge: announcement, lease of the publications, bounded pages (#684).
// Thread: worker-safe | Alloc: unrestricted
// LLM: tests of inspect::BridgeStates with an injected clock; the access rules and the wire are in ViewAssociationTests.cpp

#include <webfront/BridgeState.hpp>
#include <webfront/testing/Bdd.hpp>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace webfront::bdd;
using namespace webfront;

namespace {

using States = inspect::BridgeStates;
using Rows   = std::vector<inspect::BridgeRow>;
using namespace std::chrono_literals;

/// A page that announces itself: how many times it was asked to publish, and whether its connection is there.
struct Page {
    int  asked = 0;
    bool alive = true;

    bool announce(States& states, const std::string& host, States::Clock::time_point now) {
        return states.announce(
            host,
            this,
            [this] {
                ++asked;
            },
            [this] {
                return alive;
            },
            now);
    }
};

Rows someRows(std::size_t count, std::size_t textBytes = 4) {
    Rows rows;
    for (std::size_t index = 0; index < count; ++index)
        rows.emplace_back("pending", "call" + std::to_string(index), std::string(textBytes, 'x'), static_cast<double>(index), 1.5);
    return rows;
}

double flagsOf(const inspect::BridgePage& page) {
    return std::get<4>(page);
}

const Scenario bridgeStateLease{
    "A page publishes the state of its bridge only while an inspector reads it (#684)",
    {"inspector", "bridge-state"},
    [] {
        given("a page that announced itself, which nobody reads", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            const bool                      wanted = page.announce(states, "host-1", start);
            then("it is told that no reader waits, and is not asked to publish", [&] {
                require(!wanted);
                requireEqual(page.asked, 0);
                requireEqual(states.sourceCount(), std::size_t{1});
            });
            when("an inspector reads its state", [&] {
                auto read = states.read("host-1", 0, 200, 32768, start + 1s);
                then("the page is known, has published nothing yet, and is to be asked to publish", [&] {
                    requireEqual(flagsOf(read.page), double{inspect::bridge_flag::source});
                    require(std::get<0>(read.page).empty());
                    requireEqual(std::get<2>(read.page), 0.0);
                    require(static_cast<bool>(read.ask));
                    read.ask();
                    requireEqual(page.asked, 1);
                });
                andWhen("it reads again at once, before the page answered", [&] {
                    const auto again = states.read("host-1", 0, 200, 32768, start + 1s + 100ms);
                    then("the page is not asked twice within a second", [&] {
                        require(!static_cast<bool>(again.ask));
                    });
                });
                andWhen("the page still has not published a second later", [&] {
                    const auto later = states.read("host-1", 0, 200, 32768, start + 2s + 100ms);
                    then("it is asked again", [&] {
                        require(static_cast<bool>(later.ask));
                    });
                });
                andWhen("the page publishes", [&] {
                    const bool goOn = states.publish("host-1", &page, someRows(3), start + 1s + 200ms);
                    then("it is told to go on: a reader read less than the lease ago", [&] {
                        require(goOn);
                    });
                    then("the next read gives the rows, their publication and its age, and asks nothing", [&] {
                        const auto next = states.read("host-1", 0, 200, 32768, start + 1s + 500ms);
                        require(std::get<0>(next.page) == someRows(3));
                        requireEqual(std::get<1>(next.page), 0.0);
                        requireEqual(std::get<2>(next.page), 1.0);
                        requireEqual(std::get<3>(next.page), 300.0);
                        requireEqual(flagsOf(next.page), double{inspect::bridge_flag::source | inspect::bridge_flag::published});
                        require(!static_cast<bool>(next.ask));
                    });
                    then("a publication made after the lease is told to stop", [&] {
                        require(states.publish("host-1", &page, someRows(1), start + 1s + inspect::bridgeLease - 1ms));
                        require(!states.publish("host-1", &page, someRows(1), start + 1s + inspect::bridgeLease));
                    });
                    then("a read after the publications stopped asks the page to start again", [&] {
                        const auto late = states.read("host-1", 0, 200, 32768, start + 60s);
                        require(static_cast<bool>(late.ask));
                        require(std::get<0>(late.page) == someRows(3));
                    });
                });
            });
        });
        given("an inspector that reads a page before it announced itself", [&] {
            States                          states;
            const States::Clock::time_point start{1000s};
            const auto                      read = states.read("host-2", 0, 200, 32768, start);
            then("no source is known and nothing is asked", [&] {
                requireEqual(flagsOf(read.page), 0.0);
                require(!static_cast<bool>(read.ask));
                requireEqual(states.sourceCount(), std::size_t{0});
            });
            when("the page announces itself within the lease", [&] {
                Page page;
                then("it is told that a reader waits", [&] {
                    require(page.announce(states, "host-2", start + 2s));
                });
            });
            when("the page announces itself after the lease", [&] {
                Page page;
                then("it is told that no reader waits", [&] {
                    require(!page.announce(states, "host-2", start + inspect::bridgeLease + 1s));
                });
            });
        });
        given("a page that published and then disconnected", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "host-3", start);
            states.publish("host-3", &page, someRows(2), start);
            page.alive      = false;
            const auto read = states.read("host-3", 0, 200, 32768, start + 1s);
            then("its state is forgotten: a read finds no source", [&] {
                requireEqual(flagsOf(read.page), 0.0);
                require(std::get<0>(read.page).empty());
                requireEqual(states.sourceCount(), std::size_t{0});
            });
            when("the page is loaded again and announces itself under the same identifier", [&] {
                Page again;
                then("it is told that the reader waits, and starts from an empty state", [&] {
                    require(again.announce(states, "host-3", start + 2s));
                    const auto fresh = states.read("host-3", 0, 200, 32768, start + 2s);
                    requireEqual(flagsOf(fresh.page), double{inspect::bridge_flag::source});
                });
            });
        });
        given("a page that an inspector reads, which announces itself again", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "host-4", start);
            static_cast<void>(states.read("host-4", 0, 200, 32768, start + 1s));
            states.publish("host-4", &page, someRows(2), start + 1s);
            then("the same connection is told that its reader still waits, and keeps the state it published", [&] {
                require(page.announce(states, "host-4", start + 2s));
                const auto kept = states.read("host-4", 0, 200, 32768, start + 2s).page;
                require(std::get<0>(kept) == someRows(2));
                requireEqual(std::get<2>(kept), 1.0);
                requireEqual(flagsOf(kept), double{inspect::bridge_flag::source | inspect::bridge_flag::published});
            });
            then("another connection under the same identifier is told that the reader waits, and starts from an empty state", [&] {
                Page reloaded;
                require(reloaded.announce(states, "host-4", start + 2s));
                const auto fresh = states.read("host-4", 0, 200, 32768, start + 2s).page;
                require(std::get<0>(fresh).empty());
                requireEqual(flagsOf(fresh), double{inspect::bridge_flag::source});
            });
            then("after the lease, it is told that no reader waits", [&] {
                require(!page.announce(states, "host-4", start + 1s + inspect::bridgeLease));
            });
        });
        given("two connections that announce themselves under one identifier", [&] {
            States                          states;
            Page                            earlier, later;
            const States::Clock::time_point start{1000s};
            earlier.announce(states, "host-5", start);
            later.announce(states, "host-5", start);
            states.publish("host-5", &later, someRows(2), start);
            then("the publication of the earlier one is refused as unavailable, and the state stays that of the later one", [&] {
                std::string code;
                try {
                    states.publish("host-5", &earlier, someRows(5), start);
                } catch (const BridgeError& error) {
                    code = error.code();
                }
                requireEqual(code, std::string{inspect::code::unavailable});
                require(std::get<0>(states.read("host-5", 0, 200, 32768, start).page) == someRows(2));
            });
        });
        given("two reads that take the lock in the reverse order of their time", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "host-8", start);
            static_cast<void>(states.read("host-8", 0, 0, 32768, start + 10s));
            static_cast<void>(states.read("host-8", 0, 0, 32768, start));
            then("the later lease is kept: a publication within it is told to go on", [&] {
                require(states.publish("host-8", &page, someRows(1), start + inspect::bridgeLease + 5s));
            });
        });
        given("a read whose time was taken just before a publication", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "host-6", start);
            states.publish("host-6", &page, someRows(1), start + 5ms);
            then("the age it gives is zero, never negative", [&] {
                requireEqual(std::get<3>(states.read("host-6", 0, 200, 32768, start).page), 0.0);
            });
        });
        given("a reader of a state of several pages while the page publishes again", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "host-7", start);
            states.publish("host-7", &page, someRows(inspect::maxBridgeRows - 1), start);
            const auto first = states.read("host-7", 0, 200, 32768, start).page;
            requireEqual(std::get<0>(first).size(), std::size_t{200});
            states.publish("host-7", &page, someRows(3), start + 250ms);
            then("the next page is that of the publication it began, not of the newer one", [&] {
                const auto second = states.read("host-7", std::get<1>(first), 200, 32768, start + 260ms).page;
                requireEqual(std::get<2>(second), std::get<2>(first));
                requireEqual(std::get<0>(second).size(), std::size_t{inspect::maxBridgeRows - 1 - 200});
                requireEqual(std::get<1>(second), 0.0);
                auto all = std::get<0>(first);
                all.insert(all.end(), std::get<0>(second).begin(), std::get<0>(second).end());
                require(all == someRows(inspect::maxBridgeRows - 1));
                requireEqual(std::get<3>(second), 260.0);
            });
            then("a new read starts on the newer publication", [&] {
                const auto latest = states.read("host-7", 0, 200, 32768, start + 260ms).page;
                require(std::get<0>(latest) == someRows(3));
                requireEqual(std::get<2>(latest), 2.0);
            });
            then("once two newer ones came, the publication it began is gone: an empty page under the latest one", [&] {
                states.publish("host-7", &page, someRows(4), start + 500ms);
                const auto gone = states.read("host-7", std::get<1>(first), 200, 32768, start + 510ms).page;
                require(std::get<0>(gone).empty());
                requireEqual(std::get<1>(gone), 0.0);
                requireEqual(std::get<2>(gone), 3.0);
            });
        });
        given("a page that did not announce itself", [&] {
            States states;
            Page   page;
            then("its publication is refused as unknown, the code on which a page announces itself again", [&] {
                std::string code;
                try {
                    states.publish("nobody", &page, someRows(1));
                } catch (const BridgeError& error) {
                    code = error.code();
                }
                requireEqual(code, std::string{inspect::code::unknown});
            });
        });
      }
};

const Scenario bridgeStateBounds{
    "The relayed state of a bridge is bounded in rows, in bytes and in pages (#684, ADR-027 section 3)",
    {"inspector", "bridge-state"},
    [] {
        given("a page that announced itself", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "host", start);
            when("it publishes more rows than a state holds", [&] {
                states.publish("host", &page, someRows(2), start);
                then("the publication is refused and the earlier state is kept", [&] {
                    checkThrows<std::invalid_argument>([&] {
                        states.publish("host", &page, someRows(inspect::maxBridgeRows + 1), start);
                    });
                    require(std::get<0>(states.read("host", 0, 200, 32768, start).page) == someRows(2));
                });
            });
            when("it publishes texts longer than the bound, with control characters", [&] {
                Rows rows;
                rows.emplace_back(std::string(40, 'k'), std::string(1000, 'n'), std::string("a\nb\x7f"), 1.0, 2.0);
                states.publish("host", &page, std::move(rows), start);
                const auto stored = std::get<0>(states.read("host", 0, 200, 32768, start).page);
                then("each text is cut to its bound and the controls are made visible", [&] {
                    requireEqual(stored.size(), std::size_t{1});
                    require(std::get<0>(stored[0]).size() <= inspect::maxKindBytes);
                    require(std::get<1>(stored[0]).size() <= inspect::maxTextBytes);
                    require(std::get<1>(stored[0]).ends_with("\xE2\x80\xA6"));
                    requireEqual(std::get<2>(stored[0]),
                                 std::string{"a\xE2\x90\x8A"
                                             "b\xE2\x90\xA1"});
                });
            });
            when("it publishes a full state of the longest rows", [&] {
                states.publish("host", &page, someRows(inspect::maxBridgeRows, inspect::maxTextBytes), start);
                then("the smallest budget still serves it, page after page, each within the budget", [&] {
                    Rows        all;
                    double      cursor = 0;
                    std::size_t pages  = 0;
                    for (;;) {
                        const auto served                                 = states.read("host", cursor, 200, inspect::minPageBytes, start).page;
                        const auto& [rows, next, publication, age, flags] = served;
                        require(!rows.empty());
                        std::size_t bytes = inspect::responseHeaderBytes;
                        for (const auto& [kind, name, text, first, second] : rows)
                            bytes += 2 + 2 * 9 + 3 + kind.size() + 3 + name.size() + 3 + text.size();
                        require(bytes <= inspect::minPageBytes);
                        all.insert(all.end(), rows.begin(), rows.end());
                        ++pages;
                        if (next == 0)
                            break;
                        require(next > cursor);
                        cursor = next;
                    }
                    require(all == someRows(inspect::maxBridgeRows, inspect::maxTextBytes));
                    require(pages > 1);
                });
                then("a page holds at most the count asked, and a count of 0 gives the publication alone", [&] {
                    const auto two = states.read("host", 0, 2, 32768, start).page;
                    requireEqual(std::get<0>(two).size(), std::size_t{2});
                    requireEqual(std::get<1>(two), double(1 * inspect::bridgeCursorStride + 2));
                    const auto none = states.read("host", 0, 0, 32768, start).page;
                    require(std::get<0>(none).empty());
                    requireEqual(std::get<2>(none), 1.0);
                });
                then("a cursor past the end gives an empty last page, and a cursor that is not a whole number is refused", [&] {
                    const auto past = states.read("host", 5000, 200, 32768, start).page;
                    require(std::get<0>(past).empty());
                    requireEqual(std::get<1>(past), 0.0);
                    checkThrows<std::invalid_argument>([&] {
                        static_cast<void>(states.read("host", 1.5, 200, 32768, start));
                    });
                    checkThrows<std::invalid_argument>([&] {
                        static_cast<void>(states.read("host", 0, -1, 32768, start));
                    });
                });
            });
        });
        given("more pages that published than the relay keeps the states of", [&] {
            States                          states;
            std::vector<Page>               pages(inspect::maxBridgeSources + 1);
            const States::Clock::time_point start{1000s};
            const auto                      name = [](std::size_t index) {
                return "host" + std::to_string(index);
            };
            for (std::size_t index = 0; index < pages.size(); ++index) {
                pages[index].announce(states, name(index), start);
                // Each page is read, later than the one before, then publishes.
                static_cast<void>(states.read(name(index), 0, 0, 32768, start + std::chrono::seconds(index)));
                states.publish(name(index), &pages[index], someRows(1), start + std::chrono::seconds(index));
            }
            then("the page read the longest ago loses its state, not its place", [&] {
                requireEqual(states.sourceCount(), pages.size());
                const auto first = states.read(name(0), 0, 200, 32768, start + 100s);
                requireEqual(flagsOf(first.page), double{inspect::bridge_flag::source});
                require(std::get<0>(first.page).empty());
                require(static_cast<bool>(first.ask));
                require(std::get<0>(states.read(name(1), 0, 200, 32768, start + 100s).page) == someRows(1));
            });
            then("a page whose lease ended and that publishes last loses its own state, not a page read since", [&] {
                // host1 is read now; host0, read the longest ago, publishes once more after its lease.
                static_cast<void>(states.read(name(1), 0, 0, 32768, start + 100s));
                require(!states.publish(name(0), &pages[0], someRows(3), start + 100s));
                require(std::get<0>(states.read(name(0), 0, 200, 32768, start + 100s).page).empty());
                require(std::get<0>(states.read(name(1), 0, 200, 32768, start + 100s).page) == someRows(1));
            });
            then("a read of that page asks it, and its next publication is kept", [&] {
                auto first = states.read(name(0), 0, 200, 32768, start + 100s);
                first.ask();
                requireEqual(pages[0].asked, 1);
                require(states.publish(name(0), &pages[0], someRows(2), start + 101s));
                require(std::get<0>(states.read(name(0), 0, 200, 32768, start + 101s).page) == someRows(2));
            });
        });
        given("a connection that announces itself under another identifier", [&] {
            States                          states;
            Page                            page;
            const States::Clock::time_point start{1000s};
            page.announce(states, "first", start);
            page.announce(states, "second", start);
            then("its earlier identifier is dropped: a connection has one page at most", [&] {
                requireEqual(states.sourceCount(), std::size_t{1});
                requireEqual(flagsOf(states.read("first", 0, 0, 32768, start).page), 0.0);
                requireEqual(flagsOf(states.read("second", 0, 0, 32768, start).page), double{inspect::bridge_flag::source});
            });
        });
        given("a page read within its lease, which disconnects, then another page announces itself", [&] {
            States                          states;
            Page                            gone, other;
            const States::Clock::time_point start{1000s};
            gone.announce(states, "host-a", start);
            static_cast<void>(states.read("host-a", 0, 0, 32768, start + 1s));
            gone.alive = false;
            other.announce(states, "host-b", start + 2s);
            then("the page is dropped, but the reader's lease waits for the page that comes back under its identifier", [&] {
                requireEqual(states.sourceCount(), std::size_t{1});
                Page back;
                require(back.announce(states, "host-a", start + 3s));
            });
        });
        given("two reads of a page not yet announced that take the lock in the reverse order of their time", [&] {
            States                          states;
            const States::Clock::time_point start{1000s};
            static_cast<void>(states.read("host-c", 0, 0, 32768, start + 10s));
            static_cast<void>(states.read("host-c", 0, 0, 32768, start));
            then("the later lease is kept for the page that announces itself", [&] {
                Page page;
                require(page.announce(states, "host-c", start + inspect::bridgeLease + 5s));
            });
        });
        given("more pages announced than the relay knows", [&] {
            States                          states;
            std::vector<Page>               pages(inspect::maxBridgeAnnouncements + 1);
            const States::Clock::time_point start{1000s};
            for (std::size_t index = 0; index < inspect::maxBridgeAnnouncements; ++index) {
                pages[index].announce(states, "host" + std::to_string(index), start);
                if (index > 0)
                    static_cast<void>(states.read("host" + std::to_string(index), 0, 0, 32768, start + std::chrono::seconds(index)));
            }
            when("one more announces itself while all of them are connected", [&] {
                then("it is refused as unavailable, and no connected page loses its place", [&] {
                    std::string code;
                    try {
                        pages.back().announce(states, "last", start + 1000s);
                    } catch (const BridgeError& error) {
                        code = error.code();
                    }
                    requireEqual(code, std::string{inspect::code::unavailable});
                    requireEqual(states.sourceCount(), inspect::maxBridgeAnnouncements);
                    requireEqual(flagsOf(states.read("host0", 0, 0, 32768, start + 1000s).page), double{inspect::bridge_flag::source});
                });
            });
            when("pages disconnected before another one announces itself", [&] {
                pages[3].alive = false;
                pages[4].alive = false;
                pages.back().announce(states, "last", start + 1000s);
                then("those are forgotten first, and no connected page loses its place", [&] {
                    requireEqual(states.sourceCount(), inspect::maxBridgeAnnouncements - 1);
                    requireEqual(flagsOf(states.read("host0", 0, 0, 32768, start + 1000s).page), double{inspect::bridge_flag::source});
                });
            });
        });
      }
};

}  // namespace
