// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — tests/unit
// Role: The registry of the watches and its discovery page (ADR-027, section 3), their handle, kinds and bounded texts.
// Thread: worker-safe | Alloc: unrestricted
// LLM: unit tests of include/webfront/Watches.hpp; the bridge side is in ViewAssociationTests.cpp

#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#include <bitset>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace webfront::bdd;
using namespace webfront;

namespace {
using inspect::WatchRegistry;

std::uint64_t addWatch(WatchRegistry& registry, std::string name) {
    return registry.add(std::move(name), "integer", "app.cpp:1", false, nullptr);
}

std::vector<std::string> namesOf(const inspect::WatchPage& page) {
    std::vector<std::string> names;
    for (const auto& descriptor : std::get<0>(page))
        names.push_back(std::get<1>(descriptor));
    return names;
}

struct Unprintable { };

const std::vector<int>& sharedValues() {
    static const std::vector<int> values{1, 2};
    return values;
}
}  // namespace

const Scenario watchTexts{"The texts of a watch are bounded and made visible (ADR-027, sections 3 and 4)", {"inspector"}, [] {
                              given("texts of the application", [&] {
                                  then("control characters become their visible pictures", [&] {
                                      requireEqual(inspect::boundedText("a\nb\x7f"), std::string{"a␊b␡"});
                                  });
                                  then("a short text is kept whole", [&] {
                                      requireEqual(inspect::boundedText("grid"), std::string{"grid"});
                                  });
                                  then("a long text is cut on a character boundary and ends with an ellipsis", [&] {
                                      const std::string long_(255, 'x');
                                      const auto        cut = inspect::boundedText(long_ + "éé");  // the é crosses 256 bytes
                                      require(cut.size() <= inspect::maxTextBytes);
                                      require(cut.ends_with("…"));
                                      requireEqual(inspect::utf8Prefix("aé", 2), std::string_view{"a"});
                                  });
                                  then("a long location keeps the end of the path", [&] {
                                      const auto text = inspect::locationText(std::source_location::current());
                                      require(text.size() <= inspect::maxTextBytes);
                                      require(text.contains("WatchTests.cpp:"));
                                  });
                              });
                          }};

const Scenario watchKinds{"The kind of a watch's root follows the order of ADR-027, section 4", {"inspector"}, [] {
                              then("scalars, strings and bitsets", [&] {
                                  requireEqual(inspect::kindOf<bool>(), std::string_view{"bool"});
                                  requireEqual(inspect::kindOf<const int&>(), std::string_view{"integer"});
                                  requireEqual(inspect::kindOf<double>(), std::string_view{"real"});
                                  requireEqual(inspect::kindOf<std::string>(), std::string_view{"string"});
                                  requireEqual(inspect::kindOf<std::bitset<8>>(), std::string_view{"bitset"});
                              });
                              then("wrappers, containers, formattable and opaque types", [&] {
                                  requireEqual(inspect::kindOf<std::optional<int>>(), std::string_view{"optional"});
                                  requireEqual(inspect::kindOf<std::variant<int, float>>(), std::string_view{"variant"});
                                  requireEqual(inspect::kindOf<std::pair<int, int>>(), std::string_view{"tuple"});
                                  requireEqual(inspect::kindOf<std::tuple<int>>(), std::string_view{"tuple"});
                                  requireEqual(inspect::kindOf<std::map<int, int>>(), std::string_view{"map"});
                                  requireEqual(inspect::kindOf<std::vector<int>>(), std::string_view{"sequence"});
                                  requireEqual(inspect::kindOf<std::deque<std::string>>(), std::string_view{"sequence"});
                                  requireEqual(inspect::kindOf<const char*>(), std::string_view{"formatted"});
                                  requireEqual(inspect::kindOf<Unprintable>(), std::string_view{"opaque"});
                              });
                              then("a source's object and whether it writes come from its form", [&] {
                                  int  value     = 0;
                                  auto reader    = [](inspect::Visit<const int>) { };
                                  auto writer    = [](inspect::Visit<std::vector<int>>) { };
                                  auto reference = [&value]() -> int& {
                                      return value;
                                  };
                                  auto copy = [] {
                                      return std::string{};
                                  };
                                  using ReaderShape = inspect::SourceShape<decltype(reader)>;
                                  static_assert(std::is_same_v<ReaderShape::Object, const int> && !ReaderShape::writable);
                                  static_assert(inspect::SourceShape<decltype(writer)>::writable);
                                  // A reference read on the server thread is not writable, nor is a copy (section 6).
                                  static_assert(!inspect::SourceShape<decltype(reference)>::writable);
                                  static_assert(std::is_same_v<inspect::SourceShape<decltype(copy)>::Object, std::string>);
                                  static_assert(!inspect::WatchSource<int>);
                                  // A function given by name is a source too, kept as a pointer.
                                  static_assert(inspect::WatchSource<decltype(sharedValues)>);
                                  static_assert(std::is_same_v<inspect::SourceShape<std::decay_t<decltype(sharedValues)>>::Object, const std::vector<int>>);
                                  require(value == 0);
                              });
                          }};

const Scenario watchRegistry{"The registry of the watches gives stable identifiers and a revision (ADR-027, section 3)", {"inspector"}, [] {
                                 given("a registry with three watches", [&] {
                                     WatchRegistry registry;
                                     const auto    initial = registry.revision();
                                     const auto    a = addWatch(registry, "a"), b = addWatch(registry, "b"), c = addWatch(registry, "c");
                                     then("identifiers follow the registrations and the revision changed at each", [&] {
                                         require(a < b && b < c);
                                         requireEqual(registry.revision(), initial + 3);
                                     });
                                     then("a second watch of the same name, or an empty or long name, is refused", [&] {
                                         bool duplicate = false, empty = false, long_ = false;
                                         try {
                                             static_cast<void>(addWatch(registry, "b"));
                                         } catch (const std::invalid_argument&) {
                                             duplicate = true;
                                         }
                                         try {
                                             static_cast<void>(addWatch(registry, ""));
                                         } catch (const std::invalid_argument&) {
                                             empty = true;
                                         }
                                         try {
                                             static_cast<void>(addWatch(registry, std::string(257, 'x')));
                                         } catch (const std::invalid_argument&) {
                                             long_ = true;
                                         }
                                         require(duplicate && empty && long_);
                                         requireEqual(registry.size(), std::size_t{3});
                                     });
                                     when("one is removed and registered again under its name", [&] {
                                         registry.remove(b);
                                         const auto again = addWatch(registry, "b");
                                         then("it gets a new identifier, and the old one is gone", [&] {
                                             require(again > c);
                                             require(!registry.contains(b));
                                             requireEqual(registry.revision(), initial + 5);
                                         });
                                         then("removing an unknown identifier changes nothing", [&] {
                                             const auto revision = registry.revision();
                                             registry.remove(b);
                                             requireEqual(registry.revision(), revision);
                                         });
                                     });
                                 });
                             }};

const Scenario watchPages{
    "The discovery page lists the live watches by identifier, within the budget (ADR-027, section 3)",
    {"inspector"},
    [] {
        given("a registry of five watches", [&] {
            WatchRegistry              registry;
            std::vector<std::uint64_t> ids;
            for (const auto* name : {"a", "b", "c", "d", "e"})
                ids.push_back(addWatch(registry, name));
            const auto revision = static_cast<double>(registry.revision());
            then("a page with a count of 0 gives only the revision and whether it changed", [&] {
                const auto [list, next, current, changed] = registry.page(0, 0, 0, 32768);
                require(list.empty());
                requireEqual(next, 0.0);
                requireEqual(current, revision);
                require(changed);
                require(!std::get<3>(registry.page(revision, 0, 0, 32768)));
            });
            then("pages follow the identifiers, and the last one ends with 0", [&] {
                const auto first = registry.page(revision, 0, 2, 32768);
                requireEqual(namesOf(first), std::vector<std::string>{"a", "b"});
                requireEqual(std::get<1>(first), static_cast<double>(ids[1]));
                const auto last = registry.page(revision, static_cast<double>(ids[3]), 2, 32768);
                requireEqual(namesOf(last), std::vector<std::string>{"e"});
                requireEqual(std::get<1>(last), 0.0);
            });
            then("a removal and a registration during the listing skip and repeat nothing", [&] {
                const auto first = registry.page(revision, 0, 2, 32768);
                registry.remove(ids[0]);  // already served
                registry.remove(ids[3]);  // not served yet
                static_cast<void>(addWatch(registry, "f"));
                const auto rest = registry.page(revision, std::get<1>(first), 10, 32768);
                requireEqual(namesOf(rest), std::vector<std::string>{"c", "e", "f"});
                require(std::get<3>(rest));  // the registry changed meanwhile
            });
            then("a page holds no more than the budget, and the next one resumes after it", [&] {
                // Each descriptor takes 2 + 9 + 4 + 10 + 12 + 1 bytes: 38, and the header 64.
                const auto narrow = registry.page(revision, 0, 200, inspect::responseHeaderBytes + 80);
                requireEqual(namesOf(narrow), std::vector<std::string>{"a", "b"});
                const auto next = registry.page(revision, std::get<1>(narrow), 200, inspect::responseHeaderBytes + 80);
                requireEqual(namesOf(next), std::vector<std::string>{"c", "d"});
            });
            then("a count above 1 000 is read as 1 000, and numbers that are not whole are refused", [&] {
                requireEqual(std::get<0>(registry.page(revision, 0, 5000, 32768)).size(), std::size_t{5});
                for (const double bad : {-1.0, 0.5, 1e300})
                    checkThrows<std::invalid_argument>([&] {
                        static_cast<void>(registry.page(bad, 0, 0, 32768));
                    });
                checkThrows<std::invalid_argument>([&] {
                    static_cast<void>(registry.page(revision, 0, 2.5, 32768));
                });
            });
        });
        given("a watch whose texts are at their bounds", [&] {
            WatchRegistry registry;
            static_cast<void>(registry.add(std::string(inspect::maxTextBytes, 'n'), "formatted", std::string(inspect::maxTextBytes, 'l'), true, nullptr));
            then("a page of it fits the smallest budget", [&] {
                requireEqual(std::get<0>(registry.page(0, 0, 1, inspect::minPageBytes)).size(), std::size_t{1});
            });
        });
        given("watches whose names hold control characters", [&] {
            WatchRegistry registry;
            static_cast<void>(addWatch(registry, "line\none"));
            // Its visible form is another name: the names the application gave are what must be unique.
            static_cast<void>(addWatch(registry, "line\u240Aone"));
            static_cast<void>(addWatch(registry, std::string(inspect::maxTextBytes - 1, 'n') + "\t"));  // the picture crosses the bound
            const auto names = namesOf(registry.page(0, 0, 10, 32768));
            then("the page shows them with their control characters visible", [&] {
                requireEqual(names[0], std::string{"line\u240Aone"});
                requireEqual(names[1], std::string{"line\u240Aone"});
            });
            then("a name that its visible form makes longer is cut within the bound", [&] {
                require(names[2].size() <= inspect::maxTextBytes);
                require(names[2].ends_with("\u2026"));
            });
            then("a second watch of the same name is still refused", [&] {
                checkThrows<std::invalid_argument>([&] {
                    static_cast<void>(addWatch(registry, "line\none"));
                });
            });
        });
    }};

const Scenario watchHandles{"A watch exists while its handle lives, and the handle outlives the application (ADR-027, section 5)", {"inspector"}, [] {
                                given("a handle on a registry", [&] {
                                    auto           registry = std::make_shared<WatchRegistry>();
                                    auto           source   = std::make_shared<int>(7);
                                    std::weak_ptr  observed = source;
                                    inspect::Watch handle{registry, registry->add("grid", "integer", "app.cpp:1", false, std::move(source))};
                                    then("the watch is registered and the registry keeps its source", [&] {
                                        require(static_cast<bool>(handle));
                                        require(registry->contains(handle.id()));
                                        require(!observed.expired());
                                    });
                                    when("the handle is moved, then reset", [&] {
                                        const auto     id    = handle.id();
                                        inspect::Watch moved = std::move(handle);
                                        require(!handle);
                                        require(moved.id() == id && registry->contains(id));
                                        moved.reset();
                                        then("the watch and its source are gone", [&] {
                                            require(!registry->contains(id));
                                            require(observed.expired());
                                        });
                                    });
                                    when("the registry goes first", [&] {
                                        registry.reset();
                                        then("the handle is destroyed without touching it", [&] {
                                            handle.reset();
                                            require(!handle);
                                        });
                                    });
                                });
                                given("an application outside the developer environment", [&] {
                                    BasicWF<NetProvider, fs::DefaultFS> app{WebFrontConfig{.port = "0"}};
                                    int                                 value = 0;
                                    then("watch() registers nothing and returns an empty handle", [&] {
                                        auto handle = app.watch("value", [&value]() -> const int& {
                                            return value;
                                        });
                                        require(!handle);
                                        require(!app.watch("byName", sharedValues));  // a function given by name
                                        requireEqual(handle.id(), std::uint64_t{0});
                                    });
                                    then("an invalid name is refused all the same", [&] {
                                        checkThrows<std::invalid_argument>([&] {
                                            static_cast<void>(app.watch("", [&value]() -> const int& {
                                                return value;
                                            }));
                                        });
                                    });
                                    then("cppFunction refuses the names reserved to the inspectors", [&] {
                                        checkThrows<std::invalid_argument>([&] {
                                            app.cppFunction("webfront.inspector.containers", [] {
                                                return true;
                                            });
                                        });
                                        checkThrows<std::invalid_argument>([&] {
                                            app.cppFunction<bool>("webfront.inspector.watches", [] {
                                                return true;
                                            });
                                        });
                                        require(!app.hasCppFunction("webfront.inspector.containers"));
                                        require(app.cppFunction("webfront.inspectorish", [] {
                                            return true;
                                        }));
                                    });
                                });
                            }};
