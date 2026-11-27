// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: Writes through a watch, its executor and the destruction of its handle (ADR-027, sections 5 and 6).
// Thread: worker-safe | Alloc: unrestricted
// LLM: unit tests of Memory::write and of WatchRegistry's lifecycle; the process that must terminate is WatchTerminates.cpp

#include "WatchHarness.hpp"
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/weblink/Messages.hpp>

#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <future>
#include <optional>
#include <thread>

using namespace webfront::bdd;
using namespace webfront;
using namespace std::chrono_literals;

namespace watch_write {
using namespace watch_fixture;

struct Player {
    std::string name;
    int         score = 0;
};
struct Game {
    forms_fixture::Settings settings;
    int                     turn = 0;
};

/// A keyed paged source of scores.
struct Scores {
    std::vector<Player>       players;
    [[nodiscard]] std::size_t size() const {
        return players.size();
    }
    void visit(std::size_t first, std::size_t count, auto&& each) {
        for (std::size_t index = first; index < players.size() && index - first < count; ++index)
            each(players[index].score);
    }
    [[nodiscard]] std::string key(std::size_t position) const {
        return players.at(position).name;
    }
    [[nodiscard]] std::optional<std::size_t> find(std::string_view wanted) const {
        for (std::size_t index = 0; index < players.size(); ++index)
            if (players[index].name == wanted)
                return index;
        return std::nullopt;
    }
};

/// Lets a test hold an operation inside its source, and know that it is there.
struct Gate {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    inside = false;
    bool                    open   = false;

    void pass() {
        std::unique_lock lock{mutex};
        inside = true;
        changed.notify_all();
        changed.wait(lock, [this] {
            return open;
        });
    }
    void waitInside() {
        std::unique_lock lock{mutex};
        changed.wait(lock, [this] {
            return inside;
        });
    }
    void release() {
        std::scoped_lock lock{mutex};
        open = true;
        changed.notify_all();
    }
};

using Small = http::CustomBuffersPolicy<8192, 8192, 32768, 8192>;

/// A thread that runs a loop, as the thread that owns an object does: the executor of a watch.
struct LoopThread {
    networking::ex::run_loop loop;
    std::thread              thread;

    void start() {
        thread = std::thread{[this] {
            loop.run();
        }};
    }
    ~LoopThread() {
        loop.finish();
        if (thread.joinable())
            thread.join();
    }
};

template <typename Result>
Result await(ResultTask<Result> task) {
    return std::get<0>(*networking::ex::sync_wait(std::move(task)));
}
}  // namespace watch_write

template <>
struct webfront::inspect::Described<watch_write::Game> {
    static webfront::forms::Description<watch_write::Game> description() {
        return {.name = "Game", .fields = {webfront::forms::field("turn", &watch_write::Game::turn, "Turn", webfront::forms::Bounds{0, 100})}};
    }
};

namespace watch_write {

const Scenario watchWrites{"A value is written only where the application allows it, and only if it still shows what was displayed (ADR-027, section 6)",
                           {"inspector"},
                           [] {
                               Bench bench;
                               using Board = std::tuple<std::vector<int>, std::string, bool, std::bitset<4>, double, std::pair<int, int>>;
                               Board board{
                                   {1, 2, 3},
                                   "name",
                                   false,
                                   std::bitset<4>{"0001"},
                                   1.5,
                                   {7, 8}
                               };
                               const auto id    = bench.watch("board", visiting(board));
                               auto       page  = bench.open(id);
                               page             = bench.children(id, "/#0", revisionOf(page));
                               const auto write = [&](std::string_view path, std::string_view expected, std::string_view text) {
                                   const auto answer = bench.write(id, revisionOf(page), path, expected, text);
                                   std::get<2>(page) = std::get<0>(answer);  // the inspector adopts the revision of every answer
                                   return answer;
                               };
                               then("an accepted write changes the object, the memory, and gives a new revision", [&] {
                                   const auto before = revisionOf(page);
                                   const auto answer = write("/#0/#1", "2", "20");
                                   requireEqual(std::get<1>(answer), std::string{});
                                   require(std::get<0>(answer) != before);
                                   requireEqual(std::get<0>(board)[1], 20);
                                   require(nodesOf(bench.changes(id, std::get<0>(answer))).empty());  // the memory already holds the new value
                               });
                               then("each writable kind parses its own text", [&] {
                                   checkEqual(std::get<1>(write("/#1", "name", "other\nname")), std::string{});
                                   checkEqual(std::get<1>(board), std::string{"other\nname"});
                                   // The value now shows a control character as its picture: writing what is shown would replace it, so the node
                                   // is no longer writable.
                                   checkEqual(codeOf([&] {
                                                  return bench.write(id, revisionOf(page), "/#1", "other\xE2\x90\x8Aname", "x");
                                              }),
                                              std::string{code::readOnly});
                                   checkEqual(std::get<1>(write("/#2", "false", "true")), std::string{});
                                   check(std::get<2>(board));
                                   checkEqual(std::get<1>(write("/#3", "0001", "1010")), std::string{});
                                   checkEqual(std::get<3>(board).to_string(), std::string{"1010"});
                                   checkEqual(std::get<1>(write("/#4", "1.5", "-0.25")), std::string{});
                                   checkEqual(std::get<4>(board), -0.25);
                               });
                               then("a text that is not a value of the node rejects, and nothing changes", [&] {
                                   const auto memory = bench.memory(id);
                                   for (const auto& [path, expected, text] : std::vector<std::tuple<std::string, std::string, std::string>>{
                                            {"/#0/#1",     "2",         "two"},
                                            {"/#0/#1",     "2", "99999999999"},
                                            {"/#0/#1",     "2",            ""},
                                            {   "/#2", "false",         "yes"},
                                            {   "/#3",  "0001",         "101"},
                                            {   "/#3",  "0001",       "10102"},
                                            {   "/#4",   "1.5",         "nan"}
                                   })
                                       checkEqual(codeOf([&] {
                                                      return bench.write(id, revisionOf(page), path, expected, text);
                                                  }),
                                                  std::string{code::parse});
                                   requireEqual(static_cast<double>(memory->revision()), revisionOf(page));
                                   requireEqual(std::get<0>(board)[1], 2);
                               });
                               then("a node that is not writable rejects", [&] {
                                   for (const std::string_view path : {"", "/#0", "/#5"})
                                       checkEqual(codeOf([&] {
                                                      return bench.write(id, revisionOf(page), path, "", "1");
                                                  }),
                                                  std::string{code::readOnly});
                                   checkEqual(codeOf([&] {
                                                  return bench.write(id, revisionOf(page), "/#5/#0", "7", "1");  // not in the memory: its parent is folded
                                              }),
                                              std::string{code::unknown});
                               });
                               then("a text longer than the bound rejects", [&] {
                                   checkEqual(codeOf([&] {
                                                  return bench.write(id, revisionOf(page), "/#1", "name", std::string(bench.limits.maxWriteTextBytes + 1, 'x'));
                                              }),
                                              std::string{code::tooLarge});
                                   checkEqual(std::get<1>(write("/#1", "name", std::string(bench.limits.maxWriteTextBytes, 'x'))), std::string{});
                                   requireEqual(std::get<1>(board).size(), bench.limits.maxWriteTextBytes);
                               });
                               then("an insertion before the target makes the write stale: an ancestor no longer shows what it did", [&] {
                                   std::get<0>(board).insert(std::get<0>(board).begin(), 0);  // /#0/#1 now names the former first element
                                   const auto answer = write("/#0/#1", "2", "20");
                                   requireEqual(std::get<1>(answer), std::string{code::stale});
                                   requireEqual(std::get<0>(board), std::vector<int>({0, 1, 2, 3}));
                               });
                               then("a target that was replaced makes the write stale", [&] {
                                   std::get<0>(board)[1] = 5;
                                   requireEqual(std::get<1>(write("/#0/#1", "2", "20")), std::string{code::stale});
                                   requireEqual(std::get<0>(board)[1], 5);
                                   // And so does a value the inspector did not show, whatever the memory holds.
                                   requireEqual(std::get<1>(write("/#0/#0", "100", "20")), std::string{code::stale});
                                   requireEqual(std::get<0>(board)[0], 1);
                               });
                               then("an older revision rejects the write, with no effect", [&] {
                                   const auto old = revisionOf(page);
                                   page           = bench.children(id, "/#5", old);
                                   requireEqual(codeOf([&] {
                                                    return bench.write(id, old, "/#0/#1", "2", "20");
                                                }),
                                                std::string{code::stale});
                                   requireEqual(std::get<0>(board)[1], 2);
                                   requireEqual(static_cast<double>(bench.memory(id)->revision()), revisionOf(page));
                               });
                               then("a node that left the object makes the write stale", [&] {
                                   std::get<0>(board).clear();
                                   requireEqual(std::get<1>(write("/#0/#1", "2", "20")), std::string{code::stale});
                               });
                           }};

const Scenario watchWritesDescribed{"The fields of a described type are written within their description, and a scalar root is written itself",
                                    {"inspector"},
                                    [] {
                                        Bench bench;
                                        then("a field takes a value inside its bounds, a choice one of its names", [&] {
                                            forms_fixture::Settings settings;
                                            const auto              id     = bench.watch("settings", visiting(settings));
                                            auto                    page   = bench.open(id);
                                            auto                    answer = bench.write(id, revisionOf(page), "/.0", "5", "9");
                                            requireEqual(std::get<1>(answer), std::string{});
                                            requireEqual(settings.speed, 9);
                                            for (const std::string_view text : {"11", "0", "2.5", "fast", "", "99999999999999999999999999", "-"})
                                                checkEqual(codeOf([&] {
                                                               return bench.write(id, std::get<0>(answer), "/.0", "9", text);
                                                           }),
                                                           std::string{code::parse});
                                            answer = bench.write(id, std::get<0>(answer), "/.3", "glider", "pulsar");
                                            requireEqual(std::get<1>(answer), std::string{});
                                            require(settings.pattern == forms_fixture::Pattern::pulsar);
                                            checkEqual(codeOf([&] {
                                                           return bench.write(id, std::get<0>(answer), "/.3", "pulsar", "spaceship");
                                                       }),
                                                       std::string{code::parse});
                                            answer = bench.write(id, std::get<0>(answer), "/.1", "0.25", "0.75");
                                            requireEqual(settings.density, 0.75);
                                            answer = bench.write(id, std::get<0>(answer), "/.4", "Life", "Death");
                                            requireEqual(settings.title, std::string{"Death"});
                                            requireEqual(settings.speed, 9);
                                        });
                                        then("a scalar root is written by the empty path", [&] {
                                            int        answer = 42;
                                            const auto id     = bench.watch("answer", visiting(answer));
                                            const auto page   = bench.open(id);
                                            const auto done   = bench.write(id, revisionOf(page), "", "42", "43");
                                            requireEqual(std::get<1>(done), std::string{});
                                            requireEqual(answer, 43);
                                        });
                                        then("an element of a paged source is written by its key, wherever it moved", [&] {
                                            Scores scores{
                                                .players = {{"ada", 1}, {"bob", 2}, {"cy", 3}}
                                            };
                                            const auto id   = bench.watch("scores", visiting(scores));
                                            const auto page = bench.open(id);
                                            requireEqual(pathsOf(page), std::vector<std::string>{"", "/kada", "/kbob", "/kcy"});
                                            std::swap(scores.players[0], scores.players[2]);  // bob stays second, ada is now last
                                            const auto answer = bench.write(id, revisionOf(page), "/kada", "1", "100");
                                            requireEqual(std::get<1>(answer), std::string{});
                                            requireEqual(scores.players[2].name, std::string{"ada"});
                                            requireEqual(scores.players[2].score, 100);
                                            requireEqual(scores.players[0].score, 3);
                                        });
                                    }};

const Scenario watchMutators{"The application's mutator applies a write its own way, or refuses it (ADR-027, section 6)", {"inspector"}, [] {
                                 Bench            bench;
                                 std::vector<int> values{1, 2, 3};
                                 then("it receives the object, the path and the text, in place of the assignment", [&] {
                                     std::string seenPath, seenText;
                                     const auto  id     = bench.watch("values",
                                                                      visiting(values),
                                                                      [&](std::vector<int>& object, std::string_view path, std::string_view text) {
                                                                     seenPath = path;
                                                                     seenText = text;
                                                                     object.assign(3, 7);  // its own way: keeps an invariant of the application
                                                                      });
                                     const auto  page   = bench.open(id);
                                     const auto  answer = bench.write(id, revisionOf(page), "/#1", "2", "5");
                                     requireEqual(std::get<1>(answer), std::string{});
                                     requireEqual(seenPath, std::string{"/#1"});
                                     requireEqual(seenText, std::string{"5"});
                                     requireEqual(values, std::vector<int>({7, 7, 7}));
                                     // The memory holds what the target shows now; the other nodes are found by the next pass.
                                     requireEqual(pathsOf(bench.changes(id, std::get<0>(answer))), std::vector<std::string>({"/#0", "/#2"}));
                                     // The text was parsed for the node first: the mutator is not called for a text that is not a number.
                                     seenText.clear();
                                     checkEqual(codeOf([&] {
                                                    return bench.write(id, std::get<0>(answer), "/#1", "7", "seven");
                                                }),
                                                std::string{code::stale});  // the revision moved with the pass: refused before anything
                                     require(seenText.empty());
                                 });
                                 then("a text that is not a value of a described field is refused before the mutator is called", [&] {
                                     Game       game;
                                     int        calls = 0;
                                     const auto id    = bench.watch("game", visiting(game), [&](Game&, std::string_view, std::string_view) {
                                         ++calls;
                                     });
                                     const auto page  = bench.open(id);
                                     for (const std::string_view text : {"", "-", "1e", "101", "99999999999999999999999999"})
                                         checkEqual(codeOf([&] {
                                                        return bench.write(id, revisionOf(page), "/.0", "0", text);
                                                    }),
                                                    std::string{code::parse});
                                     requireEqual(calls, 0);
                                     requireEqual(std::get<1>(bench.write(id, revisionOf(page), "/.0", "0", "42")), std::string{});
                                     requireEqual(calls, 1);
                                 });
                                 then("a message of 1 MiB is cut to 256 bytes on a character, and the answer fits the smallest budget", [&] {
                                     // 254 bytes then characters of 3 bytes: the first of them straddles the bound.
                                     std::string message(254, 'm');
                                     while (message.size() < 1024 * 1024)
                                         message += "\xE2\x82\xAC";
                                     const auto id     = bench.watch("values", visiting(values), [&](std::vector<int>&, std::string_view, std::string_view) {
                                         throw std::runtime_error(message);
                                     });
                                     const auto page   = bench.open(id);
                                     const auto answer = bench.write(id, revisionOf(page), "/#1", "2", "5");
                                     requireEqual(std::get<1>(answer), std::string{code::refused});
                                     const auto& text = std::get<2>(answer);
                                     require(text.size() <= inspect::maxTextBytes);
                                     requireEqual(text, std::string(253, 'm') + "\xE2\x80\xA6");  // back to a whole character, then the ellipsis
                                     require(std::get<0>(answer) != revisionOf(page));
                                     requireEqual(values[1], 2);
                                     const auto bytes = 8 + 2 + 9 + inspect::detail::stringBytes(std::get<1>(answer).size())
                                                        + inspect::detail::stringBytes(text.size());
                                     require(bytes <= inspect::minPageBytes);
                                     // The revision was received: the next call is not a resynchronization.
                                     require(!resyncOf(bench.changes(id, std::get<0>(answer))));
                                 });
                             }};

const Scenario watchWriteRequestBudget{
    "A write request fits the message size, its text bounded on both sides (ADR-027, section 3, \"Budget d'une requête\")",
    {"inspector"},
    [] {
        const auto limits = inspect::limitsOf<Small>();
        then("with every argument at its largest, the request is exactly the largest message", [&] {
            msg::FunctionCall<Small>                     call;
            websocket::Frame<networking::NetworkingMock> frame{std::span(reinterpret_cast<const std::byte*>(call.header().data()), call.header().size())};
            const std::string path(inspect::maxPathBytes, 'p'), expected(inspect::maxTextBytes, 'e'), text(limits.maxWriteTextBytes, 't');
            call.encodeParameter(std::string_view{"webfront.inspector.write"}, frame);
            call.encodeParameter(9007199254740992.0, frame);
            call.encodeParameter(9007199254740992.0, frame);
            call.encodeParameter(path, frame);
            call.encodeParameter(expected, frame);
            call.encodeParameter(text, frame);
            requireEqual(call.header().size() + call.getPayloadSize(), http::maxMessageSize<Small>());
            requireEqual(inspect::requestBudget<Small>, http::maxMessageSize<Small>());
        });
        then("C++ refuses a text one byte too long, whatever the page checked", [&] {
            Bench bench;
            bench.limits     = limits;
            std::string text = "t";
            const auto  id   = bench.watch("text", visiting(text));
            const auto  page = bench.open(id);
            requireEqual(codeOf([&] {
                             return bench.write(id, revisionOf(page), "", "t", std::string(limits.maxWriteTextBytes + 1, 'x'));
                         }),
                         std::string{code::tooLarge});
            requireEqual(std::get<1>(bench.write(id, revisionOf(page), "", "t", std::string(limits.maxWriteTextBytes, 'x'))), std::string{});
            requireEqual(std::get<0>(inspect::WatchRegistry::limits(limits)), static_cast<double>(limits.maxWriteTextBytes));
            requireEqual(std::get<1>(inspect::WatchRegistry::limits(limits)), 8192.0);
            requireEqual(std::get<2>(inspect::WatchRegistry::limits(limits)), 10'000.0);
        });
    }};

const Scenario watchRegistrations{
    "A watch is known by an identifier that is never reused (ADR-027, section 3, \"Découverte\")",
    {"inspector"},
    [] {
        Bench bench;
        int   value = 1;
        then("a watch removed then declared again under its name is another watch: calls to the old one are refused", [&] {
            const auto first = bench.watch("value", visiting(value));
            const auto page  = bench.open(first);
            bench.registry->remove(first);
            const auto second = bench.watch("value", visiting(value));
            require(second != first);
            // Each call of the inspectors, on the watch that is gone.
            const auto refused = [&](auto&& call) {
                checkEqual(codeOf([&] {
                               call();
                               return 0;
                           }),
                           std::string{code::unknown});
            };
            const auto old = revisionOf(page);
            refused([&] {
                static_cast<void>(bench.children(first, "", old));
            });
            refused([&] {
                static_cast<void>(bench.changes(first, old));
            });
            refused([&] {
                static_cast<void>(bench.fold(first, "", old));
            });
            refused([&] {
                static_cast<void>(bench.reset(first));
            });
            refused([&] {
                static_cast<void>(bench.value(first, ""));
            });
            refused([&] {
                static_cast<void>(bench.write(first, old, "", "1", "2"));
            });
            requireEqual(value, 1);
            requireEqual(nodesOf(bench.open(second)).size(), std::size_t{1});
        });
        then("removing a watch frees its memories and its snapshot, for every connection", [&] {
            std::string text(20'000, 't');
            bench.limits.responseBudget = inspect::minPageBytes;
            const auto id               = bench.watch("text", visiting(text));
            const auto other            = bench.registry->session(2);
            static_cast<void>(bench.open(id));
            static_cast<void>(
                bench.registry->children(other, static_cast<double>(id), "", bench.registry->reset(other, static_cast<double>(id)), 0, 10, bench.limits));
            static_cast<void>(bench.value(id, ""));
            require(bench.session->hasSnapshot());
            require(bench.session->find(id) != nullptr && other->find(id) != nullptr);
            bench.registry->remove(id);
            require(!bench.session->hasSnapshot());
            require(bench.session->find(id) == nullptr && other->find(id) == nullptr);
        });
        then("a closed connection frees what was kept for it", [&] {
            const auto id = bench.watch("value", visiting(value));
            static_cast<void>(bench.open(id));
            requireEqual(bench.registry->sessionCount(), std::size_t{1});
            bench.registry->forget(1);
            requireEqual(bench.registry->sessionCount(), std::size_t{0});
            require(bench.session->find(id) == nullptr);
            requireEqual(codeOf([&] {
                             return bench.open(id);
                         }),
                         std::string{bridge_code::disconnected});
        });
    }};

const Scenario watchDestruction{"Destroying a watch's handle waits for an operation, unless the source owns its state (ADR-027, section 5)", {"inspector"}, [] {
                                    Bench bench;
                                    then("a source that refers to the application's objects: the destruction, outside the lock, waits for the operation", [&] {
                                        std::mutex       mutex;
                                        std::vector<int> values{1, 2, 3};
                                        Gate             gate;
                                        const auto       id      = bench.watch("values", [&](Visit<const std::vector<int>> visit) {
                                            std::scoped_lock lock{mutex};
                                            gate.pass();
                                            visit(values);
                                        });
                                        auto             reading = std::async(std::launch::async, [&] {
                                            return codeOf([&] {
                                                return bench.open(id);
                                            });
                                        });
                                        gate.waitInside();
                                        std::atomic<bool> removed{false};
                                        auto              removing = std::async(std::launch::async, [&] {
                                            bench.registry->remove(id);  // outside the lock of the source
                                            removed = true;
                                        });
                                        std::this_thread::sleep_for(50ms);
                                        require(!removed);  // the operation still reads the application's objects
                                        gate.release();
                                        removing.get();
                                        require(removed);
                                        // The operation ended on live objects, and its answer is the terminal refusal.
                                        requireEqual(reading.get(), std::string{code::unknown});
                                        requireEqual(bench.diagnosticCount(), std::size_t{0});
                                    });
                                    then("a destruction that waits longer than the delay says which watch, and goes on waiting", [&] {
                                        bench.registry->destructionWarning = 20ms;
                                        std::vector<int> values{1};
                                        Gate             gate;
                                        const auto       id      = bench.watch("slow", [&](Visit<const std::vector<int>> visit) {
                                            gate.pass();
                                            visit(values);
                                        });
                                        auto             reading = std::async(std::launch::async, [&] {
                                            return codeOf([&] {
                                                return bench.open(id);
                                            });
                                        });
                                        gate.waitInside();
                                        auto removing = std::async(std::launch::async, [&] {
                                            bench.registry->remove(id);
                                        });
                                        requireEventually([&] {
                                            return bench.diagnosticCount() == 1;
                                        });
                                        require(bench.lastDiagnostic().contains("'slow'"));
                                        require(bench.lastDiagnostic().contains("test.cpp:1"));
                                        require(removing.wait_for(0ms) != std::future_status::ready);
                                        gate.release();
                                        removing.get();
                                        requireEqual(reading.get(), std::string{code::unknown});
                                        requireEqual(bench.diagnosticCount(), std::size_t{1});
                                    });
                                    then("a source that owns its state: destroyed under the source's lock while an operation waits for it, without deadlock",
                                         [&] {
                                             struct State {
                                                 std::mutex       mutex;
                                                 std::vector<int> values{1, 2, 3};
                                                 bool             read = false;
                                             };
                                             auto                 state = std::make_shared<State>();
                                             std::weak_ptr<State> alive = state;
                                             std::atomic<bool>    asked{false};
                                             const auto id = bench.owning("owned", state, [&asked](State& owned, Visit<const std::vector<int>> visit) {
                                                 asked = true;
                                                 std::scoped_lock lock{owned.mutex};
                                                 owned.read = true;
                                                 visit(owned.values);
                                             });
                                             std::future<std::string> reading;
                                             {
                                                 std::scoped_lock lock{state->mutex};
                                                 reading = std::async(std::launch::async, [&] {
                                                     return codeOf([&] {
                                                         return bench.open(id);
                                                     });
                                                 });
                                                 requireEventually([&] {
                                                     return asked.load();
                                                 });
                                                 bench.registry->remove(id);  // under the lock the source takes: it does not wait
                                                 require(!bench.registry->contains(id));
                                             }
                                             state.reset();             // the operation is now the only owner of the state
                                             requireEqual(reading.get(), std::string{code::unknown});
                                             require(alive.expired());  // it lived until the operation ended, and no longer
                                         });
                                    then("a source that owns its state: destroyed from its own operation, the state lives until the operation ends", [&] {
                                        struct State {
                                            std::vector<int> values{1, 2, 3};
                                            inspect::Watch   handle;
                                            bool             finished = false;
                                        };
                                        auto                 state    = std::make_shared<State>();
                                        std::weak_ptr<State> alive    = state;
                                        bool                 finished = false;
                                        const auto           id       = bench.owning("self", state, [&finished](State& owned, Visit<std::vector<int>> visit) {
                                            owned.handle.reset();  // the handle was the last other owner of the state
                                            owned.values.push_back(4);
                                            visit(owned.values);
                                            owned.finished = true;
                                            finished       = true;
                                        });
                                        state->handle                 = inspect::Watch{bench.registry, id};
                                        state.reset();
                                        require(!alive.expired());
                                        requireEqual(codeOf([&] {
                                                         return bench.open(id);
                                                     }),
                                                     std::string{code::unknown});
                                        require(finished);
                                        require(alive.expired());
                                        require(!bench.registry->contains(id));
                                    });
                                    then("a write that was applied when the watch is removed says so in its refusal", [&] {
                                        struct State {
                                            int            value = 1;
                                            inspect::Watch handle;
                                        };
                                        auto       state = std::make_shared<State>();
                                        bool       drop  = false;
                                        const auto id = bench.registry->add(inspect::makeWatch<inspect::OwnedSourceShape<void (*)(State&, Visit<int>), State>>(
                                            "written",
                                            "test.cpp:1",
                                            +[](State& owned, Visit<int> visit) {
                                                visit(owned.value);
                                            },
                                            state,
                                            inspect::NoExecutor{},
                                            [&](int& value, std::string_view, std::string_view text) {
                                                value = std::stoi(std::string(text));
                                                if (drop)
                                                    state->handle.reset();
                                            }));
                                        state->handle = inspect::Watch{bench.registry, id};
                                        const auto page  = bench.open(id);
                                        drop             = true;
                                        const auto error = messageOf([&] {
                                            return bench.write(id, revisionOf(page), "", "1", "2");
                                        });
                                        requireEqual(error, std::string{"The watch was removed after the write"});
                                        requireEqual(state->value, 2);
                                    });
                                }};

const Scenario watchExecutors{"With an executor, a watch's operations run on the thread that owns its object, in the order asked (ADR-027, section 5)",
                              {"inspector"},
                              [] {
                                  Bench                        bench;
                                  LoopThread                   owner;
                                  std::vector<int>             values{1, 2, 3};
                                  std::atomic<int>             reads{0};
                                  std::atomic<std::thread::id> reader{};
                                  const auto                   scheduler = owner.loop.get_scheduler();
                                  using Accessor                         = std::function<std::vector<int>&()>;
                                  const auto id                          = bench.registry->add(
                                      inspect::makeWatch<inspect::SourceShape<Accessor>>("values",
                                                                                         "test.cpp:1",
                                                                                         Accessor{[&]() -> std::vector<int>& {
                                                                                             ++reads;
                                                                                             reader = std::this_thread::get_id();
                                                                                             return values;
                                                                                         }},
                                                                                         std::shared_ptr<inspect::NoState>{},
                                                                                         scheduler,
                                                                                         inspect::NoMutator{}));
                                  const auto children = [&](double revision) {
                                      return inspect::onWatch<Page>(bench.registry, static_cast<double>(id), [&, revision] {
                                          return bench.children(id, "", revision);
                                      });
                                  };
                                  then("the source and the whole operation run on the executor's thread, and a non-constant reference there is writable", [&] {
                                      owner.start();
                                      const auto revision = await(inspect::onWatch<double>(bench.registry, static_cast<double>(id), [&] {
                                          return bench.reset(id);
                                      }));
                                      const auto page     = await(children(revision));
                                      requireEqual(nodesOf(page).size(), std::size_t{4});
                                      require(reader.load() == owner.thread.get_id());
                                      requireEqual(flagsOf(nodesOf(page)[1]), flag::writable);
                                      const auto answer = await(inspect::onWatch<inspect::WriteAnswer>(bench.registry, static_cast<double>(id), [&] {
                                          return bench.write(id, revisionOf(page), "/#0", "1", "10");
                                      }));
                                      requireEqual(std::get<1>(answer), std::string{});
                                      requireEqual(values[0], 10);
                                  });
                                  then("calls sent together are run one by one, in order: the second, with the same revision, is stale", [&] {
                                      const auto revision = bench.reset(id);  // nothing runs yet: the loop has not started
                                      // Two calls with the same revision, asked one after the other while the executor's loop has not started.
                                      const auto ask = [&] {
                                          return std::async(std::launch::async, [&] {
                                              return await(children(revision));
                                          });
                                      };
                                      auto earlier = ask();
                                      std::this_thread::sleep_for(50ms);
                                      auto later = ask();
                                      std::this_thread::sleep_for(50ms);
                                      requireEqual(reads.load(), 0);  // queued on the executor, not run where they were asked
                                      owner.start();
                                      const auto answered = earlier.get();
                                      const auto stale    = later.get();
                                      require(!resyncOf(answered));
                                      requireEqual(nodesOf(answered).size(), std::size_t{4});
                                      require(resyncOf(stale));
                                  });
                                  then("an operation queued when the watch is removed is refused without calling the source", [&] {
                                      const auto revision = bench.reset(id);
                                      auto       queued   = std::async(std::launch::async, [&] {
                                          return codeOf([&] {
                                              return await(children(revision));
                                          });
                                      });
                                      std::this_thread::sleep_for(20ms);
                                      bench.registry->remove(id);  // nothing has started: it does not wait
                                      owner.start();
                                      requireEqual(queued.get(), std::string{code::unknown});
                                      requireEqual(reads.load(), 0);
                                      // And a call made afterwards is refused the same way, before it reaches the executor.
                                      requireEqual(codeOf([&] {
                                                       return await(children(revision));
                                                   }),
                                                   std::string{code::unknown});
                                  });
                              }};
}  // namespace watch_write
