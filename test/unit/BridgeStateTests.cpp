// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: The relay of the state of a page's bridge: announcement, lease of the publications, bounded pages (#684).
// Thread: single-threaded | Alloc: unrestricted
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
                    const bool goOn = states.publish("host-1", someRows(3), start + 1s + 200ms);
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
                        require(states.publish("host-1", someRows(1), start + 1s + inspect::bridgeLease - 1ms));
                        require(!states.publish("host-1", someRows(1), start + 1s + inspect::bridgeLease));
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
            states.publish("host-3", someRows(2), start);
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
        given("a page that did not announce itself", [&] {
            States states;
            then("its publication is refused as unknown", [&] {
                checkThrows<BridgeError>([&] {
                    states.publish("nobody", someRows(1));
                });
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
                states.publish("host", someRows(2), start);
                then("the publication is refused and the earlier state is kept", [&] {
                    checkThrows<std::invalid_argument>([&] {
                        states.publish("host", someRows(inspect::maxBridgeRows + 1), start);
                    });
                    require(std::get<0>(states.read("host", 0, 200, 32768, start).page) == someRows(2));
                });
            });
            when("it publishes texts longer than the bound, with control characters", [&] {
                Rows rows;
                rows.emplace_back(std::string(40, 'k'), std::string(1000, 'n'), std::string("a\nb\x7f"), 1.0, 2.0);
                states.publish("host", std::move(rows), start);
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
                states.publish("host", someRows(inspect::maxBridgeRows, inspect::maxTextBytes), start);
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
                    requireEqual(std::get<1>(two), 2.0);
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
        given("more pages than the relay keeps", [&] {
            States                          states;
            std::vector<Page>               pages(inspect::maxBridgeSources + 1);
            const States::Clock::time_point start{1000s};
            for (std::size_t index = 0; index < inspect::maxBridgeSources; ++index) {
                pages[index].announce(states, "host" + std::to_string(index), start);
                // Every page but the first is read, each later than the one before.
                if (index > 0)
                    static_cast<void>(states.read("host" + std::to_string(index), 0, 0, 32768, start + std::chrono::seconds(index)));
            }
            when("one more announces itself", [&] {
                pages.back().announce(states, "last", start + 100s);
                then("it takes the place of the page that was read the longest ago", [&] {
                    requireEqual(states.sourceCount(), inspect::maxBridgeSources);
                    requireEqual(flagsOf(states.read("host0", 0, 0, 32768, start + 100s).page), 0.0);
                    requireEqual(flagsOf(states.read("host1", 0, 0, 32768, start + 100s).page), double{inspect::bridge_flag::source});
                    requireEqual(flagsOf(states.read("last", 0, 0, 32768, start + 100s).page), double{inspect::bridge_flag::source});
                });
            });
            when("pages disconnected before another one announces itself", [&] {
                pages[3].alive = false;
                pages[4].alive = false;
                pages.back().announce(states, "last", start + 100s);
                then("those are forgotten first, and no connected page loses its place", [&] {
                    requireEqual(states.sourceCount(), inspect::maxBridgeSources - 1);
                    requireEqual(flagsOf(states.read("host0", 0, 0, 32768, start + 100s).page), double{inspect::bridge_flag::source});
                });
            });
        });
      }
};

}  // namespace
