// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: The trees of the watches (ADR-027, sections 3 to 5): kinds, paths, pages within their budget, lists, keys, full values and bounds.
// Thread: worker-safe | Alloc: unrestricted
// LLM: unit tests of WatchValues.hpp and of the reading side of WatchMemory.hpp; passes are in WatchPassTests.cpp, writes in WatchWriteTests.cpp

#include "WatchHarness.hpp"
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/weblink/Messages.hpp>

#include <bitset>
#include <format>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <variant>

using namespace webfront::bdd;
using namespace webfront;

namespace watch_tree {
using namespace watch_fixture;

/// An element of a keyed source, with values of its own.
struct KeyedItem {
    std::string      key;
    std::vector<int> values;
};
struct KeyedItems {
    std::vector<KeyedItem>    items;
    [[nodiscard]] std::size_t size() const {
        return items.size();
    }
    void visit(std::size_t first, std::size_t count, auto&& each) {
        for (std::size_t index = first; index < items.size() && index - first < count; ++index)
            each(items[index].values);
    }
    [[nodiscard]] std::string key(std::size_t position) const {
        return items.at(position).key;
    }
    [[nodiscard]] std::optional<std::size_t> find(std::string_view wanted) const {
        for (std::size_t index = 0; index < items.size(); ++index)
            if (items[index].key == wanted)
                return index;
        return std::nullopt;
    }
};

struct Point {
    int x = 0;
    int y = 0;
};
struct Opaque { };
/// Its formatter throws while `failing` is set, and writes `size` characters straight to the output otherwise.
struct Wide {
    std::size_t size    = 0;
    bool        failing = false;
};

/// A container that pages itself, with keys: the names of its items.
struct Keyed {
    std::string        name;
    std::string        text;
    std::vector<Keyed> items;
    bool               keyFails = false;
    mutable int        visits   = 0;

    [[nodiscard]] std::size_t size() const {
        return items.size();
    }
    template <typename Each>
    void visit(std::size_t first, std::size_t count, Each&& each) {
        ++visits;
        for (std::size_t index = first; index < items.size() && index - first < count; ++index)
            each(items[index]);
    }
    [[nodiscard]] std::string key(std::size_t position) const {
        if (keyFails)
            throw std::runtime_error("no key");
        return items.at(position).name;
    }
    [[nodiscard]] std::optional<std::size_t> find(std::string_view wanted) const {
        for (std::size_t index = 0; index < items.size(); ++index)
            if (items[index].name == wanted)
                return index;
        return std::nullopt;
    }
};

/// A container that pages itself without keys, and gives its elements by value (computed on demand).
struct Squares {
    std::size_t               count = 0;
    [[nodiscard]] std::size_t size() const {
        return count;
    }
    template <typename Each>
    void visit(std::size_t first, std::size_t wanted, Each&& each) const {
        for (std::size_t index = first; index < count && index - first < wanted; ++index)
            each(index * index);
    }
};

/// A keyed paged source of numbers, whose keys may repeat.
struct Row {
    std::string id;
    int         value = 0;
};
struct Rows {
    std::vector<Row>          rows;
    [[nodiscard]] std::size_t size() const {
        return rows.size();
    }
    void visit(std::size_t first, std::size_t count, auto&& each) {
        for (std::size_t index = first; index < rows.size() && index - first < count; ++index)
            each(rows[index].value);
    }
    [[nodiscard]] std::string key(std::size_t position) const {
        return rows.at(position).id;
    }
    [[nodiscard]] std::optional<std::size_t> find(std::string_view wanted) const {
        for (std::size_t index = 0; index < rows.size(); ++index)
            if (rows[index].id == wanted)
                return index;
        return std::nullopt;
    }
};

/// A sequence of itself: as deep as wanted.
struct Tree : std::vector<Tree> { };

struct Labelled {
    int value = 1;
};

using Small = http::CustomBuffersPolicy<8192, 8192, 32768, 8192>;

std::vector<std::byte> messageBytes(const websocket::Frame<networking::NetworkingMock>& frame) {
    std::vector<std::byte> bytes;
    const auto             buffers = frame.toBuffers();
    for (auto buffer = std::next(buffers.begin()); buffer != buffers.end(); ++buffer) {
        const auto* first = static_cast<const std::byte*>(buffer->data());
        bytes.insert(bytes.end(), first, first + buffer->size());
    }
    return bytes;
}
}  // namespace watch_tree

template <>
struct std::formatter<watch_tree::Point> : std::formatter<std::string_view> {
    auto format(const watch_tree::Point& point, std::format_context& context) const {
        return std::format_to(context.out(), "({}, {})", point.x, point.y);
    }
};
template <>
struct std::formatter<watch_tree::Wide> {
    constexpr auto parse(std::format_parse_context& context) {
        return context.begin();
    }
    auto format(const watch_tree::Wide& wide, std::format_context& context) const {
        if (wide.failing)
            throw std::runtime_error("formatter\nfailed");
        auto out = context.out();
        for (std::size_t written = 0; written < wide.size; ++written)
            *out++ = 'w';
        return out;
    }
};
template <>
struct webfront::inspect::Described<watch_tree::Labelled> {
    static webfront::forms::Description<watch_tree::Labelled> description() {
        return {.name = "Labelled", .fields = {webfront::forms::field("value", &watch_tree::Labelled::value, std::string(1000, 'L'))}};
    }
};

namespace watch_tree {

static_assert(inspect::PagedSource<Keyed> && inspect::KeyedPagedSource<Keyed>);
static_assert(inspect::PagedSource<const Squares> && !inspect::KeyedPagedSource<Squares>);
static_assert(!inspect::PagedSource<std::vector<int>>);
static_assert(inspect::kindOf<Keyed>() == "sequence" && inspect::kindOf<forms_fixture::Settings>() == "described");
static_assert(inspect::limitsOf<Small>().responseBudget == 8192);
static_assert(inspect::limitsOf<Small>().maxWriteTextBytes == 8192 - inspect::writeRequestFixedBytes - 3);
static_assert(inspect::limitsOf<http::DefaultBuffersPolicy>().responseBudget == 32768);

const Scenario watchValueKinds{"Each kind of value is rendered as a node (ADR-027, section 4)", {"inspector"}, [] {
                                   using Everything = std::tuple<bool,
                                                                 int,
                                                                 double,
                                                                 std::string,
                                                                 std::bitset<4>,
                                                                 std::optional<int>,
                                                                 std::variant<int, std::string>,
                                                                 std::pair<int, std::string>,
                                                                 std::map<std::string, int>,
                                                                 forms_fixture::Settings,
                                                                 Point,
                                                                 Opaque,
                                                                 std::vector<int>>;
                                   Everything all{
                                       true,
                                       -3,
                                       0.5,
                                       "a\nb",
                                       std::bitset<4>{"0101"},
                                       7,
                                       std::string{"text"},
                                       {1, "x"},
                                       {{"one", 1}, {"two", 2}},
                                       forms_fixture::Settings{},
                                       Point{1, 2},
                                       Opaque{},
                                       {4, 5}
                                   };
                                   Bench      bench;
                                   const auto id       = bench.watch("all", visiting(all));
                                   auto       page     = bench.open(id);
                                   auto       revision = revisionOf(page);
                                   const auto unfold   = [&](std::string_view path) {
                                       auto children = bench.children(id, path, revision);
                                       revision      = revisionOf(children);
                                       return children;
                                   };
                                   then("the root is the watched object, under the watch's name, and comes first", [&] {
                                       requireEqual(nodesOf(page).size(), std::size_t{14});
                                       const auto& root = nodesOf(page).front();
                                       requireEqual(pathOf(root), std::string{});
                                       requireEqual(kindOf(root), std::string{"tuple"});
                                       requireEqual(labelOf(root), std::string{"all"});
                                       requireEqual(countOf(root), std::size_t{13});
                                       requireEqual(nextOf(page), 0.0);
                                       require(!resyncOf(page));
                                   });
                                   then("scalars, strings and bitsets show their value and are writable through a Visit<T>", [&] {
                                       const auto expect = [&](std::string_view path, std::string_view kind, std::string_view value) {
                                           const auto& node = nodeAt(page, path);
                                           checkEqual(kindOf(node), std::string(kind));
                                           checkEqual(valueOf(node), std::string(value));
                                           checkEqual(flagsOf(node), flag::writable);
                                           checkEqual(countOf(node), std::size_t{0});
                                       };
                                       expect("/#0", "bool", "true");
                                       expect("/#1", "integer", "-3");
                                       expect("/#2", "real", "0.5");
                                       // A control character is made visible; the text shown is then not the object's, and is not offered to edition.
                                       checkEqual(valueOf(nodeAt(page, "/#3")),
                                                  std::string{"a\xE2\x90\x8A"
                                                              "b"});
                                       checkEqual(kindOf(nodeAt(page, "/#3")), std::string{"string"});
                                       checkEqual(flagsOf(nodeAt(page, "/#3")), 0u);
                                       expect("/#4", "bitset", "0101");
                                       checkEqual(labelOf(nodeAt(page, "/#1")), std::string{"1"});
                                   });
                                   then("optional, variant, pair and tuple have their content as children", [&] {
                                       checkEqual(valueOf(nodeAt(page, "/#5")), std::string{"engaged"});
                                       checkEqual(countOf(nodeAt(page, "/#5")), std::size_t{1});
                                       checkEqual(flagsOf(nodeAt(page, "/#5")), 0u);
                                       const auto optional = unfold("/#5");
                                       requireEqual(pathsOf(optional), std::vector<std::string>{"/#5", "/#5/#0"});
                                       checkEqual(labelOf(nodesOf(optional)[1]), std::string{"value"});
                                       checkEqual(valueOf(nodesOf(optional)[1]), std::string{"7"});
                                       checkEqual(kindOf(nodeAt(page, "/#6")), std::string{"variant"});
                                       checkEqual(valueOf(nodeAt(page, "/#6")), std::string{"alternative 1"});
                                       const auto variant = unfold("/#6");
                                       requireEqual(pathsOf(variant), std::vector<std::string>{"/#6", "/#6/#1"});  // the segment tells the alternative
                                       checkEqual(kindOf(nodesOf(variant)[1]), std::string{"string"});
                                       checkEqual(valueOf(nodesOf(variant)[1]), std::string{"text"});
                                       const auto pair = unfold("/#7");
                                       requireEqual(pathsOf(pair), std::vector<std::string>{"/#7", "/#7/#0", "/#7/#1"});
                                       checkEqual(valueOf(nodesOf(pair)[2]), std::string{"x"});
                                   });
                                   then("a sequence and an associative container have their elements as children, a map's labelled by their keys", [&] {
                                       checkEqual(kindOf(nodeAt(page, "/#8")), std::string{"map"});
                                       const auto map = unfold("/#8");
                                       requireEqual(pathsOf(map), std::vector<std::string>{"/#8", "/#8/#0", "/#8/#1"});
                                       checkEqual(labelOf(nodesOf(map)[1]), std::string{"one"});
                                       checkEqual(valueOf(nodesOf(map)[2]), std::string{"2"});
                                       checkEqual(kindOf(nodesOf(map)[2]), std::string{"integer"});
                                       const auto sequence = unfold("/#12");
                                       requireEqual(pathsOf(sequence), std::vector<std::string>{"/#12", "/#12/#0", "/#12/#1"});
                                       checkEqual(labelOf(nodesOf(sequence)[1]), std::string{"[0]"});
                                       checkEqual(valuesOf(sequence), std::vector<std::string>{"", "4", "5"});
                                   });
                                   then("a described type has one child per field, told by its rank and labelled by its description", [&] {
                                       checkEqual(kindOf(nodeAt(page, "/#9")), std::string{"described"});
                                       checkEqual(valueOf(nodeAt(page, "/#9")), std::string{"Settings"});
                                       const auto described = unfold("/#9");
                                       requireEqual(pathsOf(described), std::vector<std::string>{"/#9", "/#9/.0", "/#9/.1", "/#9/.2", "/#9/.3", "/#9/.4"});
                                       checkEqual(labelOf(nodesOf(described)[1]), std::string{"Speed"});
                                       checkEqual(kindOf(nodesOf(described)[1]), std::string{"integer"});
                                       checkEqual(valueOf(nodesOf(described)[1]), std::string{"5"});
                                       checkEqual(kindOf(nodesOf(described)[2]), std::string{"real"});
                                       checkEqual(kindOf(nodesOf(described)[3]), std::string{"bool"});
                                       checkEqual(kindOf(nodesOf(described)[4]), std::string{"enum"});
                                       checkEqual(valueOf(nodesOf(described)[4]), std::string{"glider"});
                                       checkEqual(kindOf(nodesOf(described)[5]), std::string{"string"});
                                       checkEqual(flagsOf(nodesOf(described)[4]), flag::writable);
                                   });
                                   then("a formattable type is formatted, and any other shows its type as not displayable", [&] {
                                       checkEqual(kindOf(nodeAt(page, "/#10")), std::string{"formatted"});
                                       checkEqual(valueOf(nodeAt(page, "/#10")), std::string{"(1, 2)"});
                                       checkEqual(flagsOf(nodeAt(page, "/#10")), 0u);
                                       checkEqual(kindOf(nodeAt(page, "/#11")), std::string{"opaque"});
                                       check(valueOf(nodeAt(page, "/#11")).contains("Opaque"));
                                       check(valueOf(nodeAt(page, "/#11")).ends_with("(not displayable)"));
                                   });
                               }};

const Scenario watchSourcesRead{"A watch reads through the source the application gives (ADR-027, section 5)", {"inspector"}, [] {
                                    Bench            bench;
                                    std::vector<int> values{1, 2, 3};
                                    then("a Visit<const T> shows the same tree, read-only", [&] {
                                        const auto id   = bench.watch("values", [&](Visit<const std::vector<int>> visit) {
                                            visit(values);
                                        });
                                        const auto page = bench.open(id);
                                        requireEqual(valuesOf(page), std::vector<std::string>{"", "1", "2", "3"});
                                        for (const auto& node : nodesOf(page))
                                            checkEqual(flagsOf(node), 0u);
                                        checkEqual(std::get<4>(std::get<0>(bench.registry->page(0, 0, 10, 4096)).front()), false);
                                    });
                                    then("the whole operation runs inside the visit, under the application's lock", [&] {
                                        bool       locked = false;
                                        int        reads  = 0;
                                        const auto id     = bench.watch("locked", [&](Visit<const std::list<Wide>> visit) {
                                            static const std::list<Wide> wides{
                                                Wide{.size = 1, .failing = false},
                                                Wide{.size = 2, .failing = false}
                                            };
                                            locked = true;
                                            visit(wides);
                                            locked = false;
                                            ++reads;
                                        });
                                        // The formatter of each element runs while the source holds its "lock".
                                        const auto page = bench.open(id);
                                        requireEqual(valuesOf(page), std::vector<std::string>{"", "w", "ww"});
                                        requireEqual(reads, 1);
                                        require(!locked);
                                    });
                                    then("an accessor that returns a reference or a copy is read where the call runs, and never written", [&] {
                                        const auto byReference = bench.watch("reference", [&]() -> std::vector<int>& {
                                            return values;
                                        });
                                        const auto byCopy      = bench.watch("copy", [&] {
                                            return values;
                                        });
                                        for (const auto id : {byReference, byCopy}) {
                                            const auto page = bench.open(id);
                                            requireEqual(valuesOf(page), std::vector<std::string>{"", "1", "2", "3"});
                                            checkEqual(flagsOf(nodesOf(page)[1]), 0u);
                                            requireEqual(codeOf([&] {
                                                             return bench.write(id, revisionOf(page), "/#0", "1", "9");
                                                         }),
                                                         std::string{code::readOnly});
                                        }
                                        requireEqual(values.front(), 1);
                                    });
                                    then("a source that does not give its object, or fails before giving it, rejects the call and changes nothing", [&] {
                                        bool       give   = true;
                                        bool       fail   = false;
                                        const auto id     = bench.watch("shy", [&](Visit<const std::vector<int>> visit) {
                                            if (fail)
                                                throw std::runtime_error("locked\nout");
                                            if (give)
                                                visit(values);
                                        });
                                        const auto page   = bench.open(id);
                                        const auto memory = bench.memory(id);
                                        give              = false;
                                        requireEqual(codeOf([&] {
                                                         return bench.children(id, "", revisionOf(page));
                                                     }),
                                                     std::string{code::unavailable});
                                        fail = true;
                                        requireEqual(codeOf([&] {
                                                         return bench.changes(id, revisionOf(page));
                                                     }),
                                                     std::string{"exception"});
                                        check(messageOf([&] {
                                                  return bench.changes(id, revisionOf(page));
                                              }).contains("locked\xE2\x90\x8Aout"));
                                        requireEqual(static_cast<double>(memory->revision()), revisionOf(page));
                                        requireEqual(memory->size(), std::size_t{4});
                                    });
                                    then("a source that fails after its visit is reported, and the answer is served", [&] {
                                        const auto id   = bench.watch("late", [&](Visit<const std::vector<int>> visit) {
                                            visit(values);
                                            throw std::runtime_error("after the visit");
                                        });
                                        const auto page = bench.open(id);
                                        requireEqual(nodesOf(page).size(), std::size_t{4});
                                        require(bench.lastDiagnostic().contains("'late'"));
                                        require(bench.lastDiagnostic().contains("after the visit"));
                                    });
                                    then("a wide character is shown as its number, and has no text to be written", [&] {
                                        std::tuple<char16_t, wchar_t, char32_t, char8_t, signed char, unsigned short>
                                                   characters{u'a', L'b', U'c', u8'd', static_cast<signed char>('e'), static_cast<unsigned short>(102)};
                                        const auto id   = bench.watch("characters", visiting(characters));
                                        const auto page = bench.open(id);
                                        requireEqual(valuesOf(page), std::vector<std::string>({"", "97", "98", "99", "100", "101", "102"}));
                                        for (std::size_t index = 1; index <= 4; ++index) {
                                            checkEqual(kindOf(nodesOf(page)[index]), std::string{"integer"});
                                            checkEqual(flagsOf(nodesOf(page)[index]), 0u);
                                        }
                                        checkEqual(flagsOf(nodesOf(page)[5]), flag::writable);
                                        requireEqual(std::get<1>(bench.write(id, revisionOf(page), "/#5", "102", "65535")), std::string{});
                                        requireEqual(std::get<5>(characters), 65535);
                                    });
                                    then("a paged source that gives its elements by value is read through copies", [&] {
                                        Squares    squares{.count = 1000};
                                        const auto id   = bench.watch("squares", visiting(squares));
                                        const auto page = bench.children(id, "", bench.reset(id), 0, 3);
                                        requireEqual(valuesOf(page), std::vector<std::string>{"", "0", "1", "4"});
                                        checkEqual(kindOf(nodesOf(page).front()), std::string{"sequence"});
                                        checkEqual(flagsOf(nodesOf(page)[1]), 0u);  // a copy is not the object
                                        requireEqual(nextOf(page), 3.0);
                                        const auto last = bench.children(id, "", revisionOf(page), 999, 10);
                                        requireEqual(valuesOf(last), std::vector<std::string>{"", "998001"});
                                        requireEqual(nextOf(last), 0.0);
                                    });
                                }};

const Scenario watchPaths{"Paths are ASCII, reversible and bounded (ADR-027, section 3)", {"inspector"}, [] {
                              then("a key is percent-encoded, and decoded back", [&] {
                                  const std::string key     = "a/b%c \xC3\xA9~";
                                  const auto        encoded = inspect::encodeKey(key);
                                  requireEqual(encoded, std::string{"a%2Fb%25c%20%C3%A9~"});
                                  const auto segments = inspect::parsePath("/#12/.3/k" + encoded);
                                  require(segments.has_value());
                                  requireEqual(segments->size(), std::size_t{3});
                                  require((*segments)[0] == inspect::Segment{.letter = '#', .number = 12, .key = {}});
                                  require((*segments)[1] == inspect::Segment{.letter = '.', .number = 3, .key = {}});
                                  requireEqual((*segments)[2].key, key);
                                  require(inspect::parsePath("")->empty());
                                  require(inspect::parsePath("/k")->front().key.empty());
                              });
                              then("an ill-formed path is refused", [&] {
                                  for (const auto& bad : std::vector<std::string>{"#0",
                                                                                  "/",
                                                                                  "/#0/",
                                                                                  "//#0",
                                                                                  "/#",
                                                                                  "/#01",
                                                                                  "/#-1",
                                                                                  "/#1x",
                                                                                  "/x1",
                                                                                  "/#18446744073709551616",
                                                                                  "/k%2",
                                                                                  "/k%2f",
                                                                                  "/k%41",
                                                                                  "/ka/b c",
                                                                                  "/k" + std::string(inspect::maxKeyBytes + 1, 'a'),
                                                                                  "/" + std::string(inspect::maxPathBytes, '#')})
                                      check(!inspect::parsePath(bad).has_value());
                                  std::string deep;
                                  for (std::size_t level = 0; level < inspect::maxPathSegments; ++level)
                                      deep += "/#0";
                                  require(inspect::parsePath(deep).has_value());
                                  require(!inspect::parsePath(deep + "/#0").has_value());
                                  Bench            bench;
                                  std::vector<int> values{1};
                                  const auto       id   = bench.watch("values", visiting(values));
                                  const auto       page = bench.open(id);
                                  for (const std::string_view bad : {"#0", "/#0/", "/k%41"})
                                      checkEqual(codeOf([&] {
                                                     return bench.children(id, bad, revisionOf(page));
                                                 }),
                                                 std::string{code::unknown});
                                  requireEqual(bench.memory(id)->size(), std::size_t{2});
                              });
                              then("the 64th level is not unfolded", [&] {
                                  Tree  root;
                                  Tree* leaf = &root;
                                  for (int level = 0; level < 70; ++level)
                                      leaf = &leaf->emplace_back();
                                  Bench       bench;
                                  const auto  id   = bench.watch("tree", visiting(root));
                                  auto        page = bench.open(id);
                                  std::string path;
                                  for (std::size_t level = 1; level <= inspect::maxPathSegments; ++level) {
                                      path += "/#0";
                                      requireEqual(flagsOf(nodesOf(page).back()) & flag::depthReached,
                                                   level == inspect::maxPathSegments ? flag::depthReached : 0u);
                                      if (level < inspect::maxPathSegments)
                                          page = bench.children(id, path, revisionOf(page));
                                  }
                                  requireEqual(pathOf(nodesOf(page).back()), path);
                                  requireEqual(countOf(nodesOf(page).back()), std::size_t{1});
                                  requireEqual(codeOf([&] {
                                                   return bench.children(id, path, revisionOf(page));
                                               }),
                                               std::string{code::limit});
                              });
                              then("a parent whose children's paths could pass 1 024 bytes is not unfolded", [&] {
                                  Keyed  root;
                                  Keyed* leaf = &root;
                                  for (int level = 0; level < 9; ++level) {
                                      leaf->items.push_back(
                                          Keyed{.name = std::string(inspect::maxKeyBytes, static_cast<char>('a' + level)), .text = {}, .items = {}});
                                      leaf = &leaf->items.back();
                                  }
                                  Bench       bench;
                                  const auto  id   = bench.watch("keyed", visiting(root));
                                  auto        page = bench.open(id);
                                  std::string path;
                                  std::size_t level = 0;
                                  while ((flagsOf(nodesOf(page).back()) & flag::depthReached) == 0) {
                                      path = pathOf(nodesOf(page).back());
                                      page = bench.children(id, path, revisionOf(page));
                                      ++level;
                                      require(level < 9);
                                  }
                                  const auto& last = nodesOf(page).back();
                                  requireEqual(level, std::size_t{6});  // 7 segments of 130 bytes: an eighth would pass the bound
                                  require(pathOf(last).size() <= inspect::maxPathBytes);
                                  require(pathOf(last).size() + 1 + inspect::maxKeySegment > inspect::maxPathBytes);
                                  require(inspect::detail::nodeBytes(last) <= inspect::maxNodeBytes);
                                  requireEqual(codeOf([&] {
                                                   return bench.children(id, pathOf(last), revisionOf(page));
                                               }),
                                               std::string{code::limit});
                              });
                              then("a field whose label is very long is told by its rank, its label cut, and is not writable", [&] {
                                  Labelled    labelled;
                                  Bench       bench;
                                  const auto  id   = bench.watch("labelled", visiting(labelled));
                                  const auto  page = bench.open(id);
                                  const auto& node = nodesOf(page).back();
                                  requireEqual(pathOf(node), std::string{"/.0"});
                                  require(labelOf(node).size() <= inspect::maxTextBytes);
                                  require(labelOf(node).ends_with("\xE2\x80\xA6"));
                                  requireEqual(flagsOf(node), 0u);
                              });
                          }};

const Scenario watchPagesBudget{"A page of children is paged, and reduced to the answer's budget (ADR-027, section 3)", {"inspector"}, [] {
                                    Bench bench;
                                    then("a list is read page after page, each starting with the parent", [&] {
                                        std::vector<int> values(1000);
                                        for (std::size_t index = 0; index < values.size(); ++index)
                                            values[index] = static_cast<int>(index);
                                        const auto id   = bench.watch("values", visiting(values));
                                        auto       page = bench.open(id);
                                        requireEqual(nodesOf(page).size(), std::size_t{201});
                                        requireEqual(nextOf(page), 200.0);
                                        std::size_t read = 200;
                                        while (nextOf(page) != 0) {
                                            const auto start = nextOf(page);
                                            page             = bench.children(id, "", revisionOf(page), start);
                                            requireEqual(pathOf(nodesOf(page).front()), std::string{});
                                            requireEqual(countOf(nodesOf(page).front()), std::size_t{1000});
                                            requireEqual(valueOf(nodesOf(page)[1]), std::to_string(read));
                                            read += nodesOf(page).size() - 1;
                                        }
                                        requireEqual(read, std::size_t{1000});
                                        // A window anywhere in the container is read directly, and a count is at most 1 000.
                                        page = bench.children(id, "", revisionOf(page), 990, 5000);
                                        requireEqual(nodesOf(page).size(), std::size_t{11});
                                        requireEqual(pathOf(nodesOf(page).back()), std::string{"/#999"});
                                        bench.limits.responseBudget = 1 << 20;
                                        page                        = bench.children(id, "", revisionOf(page), 0, 5000);
                                        requireEqual(nodesOf(page).size(), std::size_t{1001});
                                        page = bench.children(id, "", revisionOf(page), 0, 0);
                                        requireEqual(nodesOf(page).size(), std::size_t{1});
                                    });
                                    then("a page stops where the budget ends, and can fill it to the byte", [&] {
                                        bench.limits.responseBudget = inspect::limitsOf<Small>().responseBudget;
                                        std::vector<std::string> texts(100, std::string(100, 't'));
                                        const auto               id   = bench.watch("texts", visiting(texts));
                                        auto                     page = bench.open(id);
                                        const auto               held = nodesOf(page).size() - 1;
                                        require(held > 10 && held < 100);
                                        require(bytesOf(page) <= bench.limits.responseBudget);
                                        requireEqual(nextOf(page), static_cast<double>(held));
                                        // The last element served grows by exactly the room left: the page is then the budget, to the byte.
                                        texts[held - 1].append(bench.limits.responseBudget - bytesOf(page), 't');
                                        page = bench.open(id);
                                        requireEqual(nodesOf(page).size() - 1, held);
                                        requireEqual(bytesOf(page), bench.limits.responseBudget);
                                        // The message it makes is what the policy allows, and no more: maxMessageSize is here below the encoding buffer.
                                        msg::FunctionReturn<Small>                   message;
                                        websocket::Frame<networking::NetworkingMock> frame{
                                            std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
                                        message.encodeParameter(page, frame);
                                        const auto bytes = messageBytes(frame);
                                        requireEqual(bytes.size(), http::maxMessageSize<Small>());
                                        auto          decoded = msg::FunctionReturn<Small>::castFromRawData(bytes);
                                        auto          payload = decoded->payload();
                                        inspect::Page read;
                                        decoded->decodeParameter(read, payload);
                                        require(payload.empty());
                                        require(read == page);
                                        // One byte more, and the element waits for the next page.
                                        texts[held - 1] += 't';
                                        page             = bench.open(id);
                                        requireEqual(nodesOf(page).size() - 1, held - 1);
                                        requireEqual(nextOf(page), static_cast<double>(held - 1));
                                    });
                                    then("the smallest budget still holds a parent and a child whose every field is at its bound", [&] {
                                        bench.limits.responseBudget = inspect::minPageBytes;
                                        Keyed root;
                                        for (int index = 0; index < 4; ++index)
                                            root.items.push_back(
                                                Keyed{.name = std::string(inspect::maxKeyBytes, static_cast<char>('a' + index)), .text = {}, .items = {}});
                                        const auto id   = bench.watch(std::string(inspect::maxTextBytes, 'n'), visiting(root));
                                        auto       page = bench.open(id);
                                        require(nodesOf(page).size() >= 2);
                                        require(bytesOf(page) <= inspect::minPageBytes);
                                    });
                                }};

const Scenario watchMemoryLimit{"The memory of a connection for a watch holds 10 000 nodes at most (ADR-027, section 3)", {"inspector"}, [] {
                                    Bench            bench;
                                    std::vector<int> values(30'000, 1);
                                    const auto       id   = bench.watch("values", visiting(values));
                                    auto             page = bench.open(id, 1000);
                                    std::string      refused;
                                    while (nextOf(page) != 0 && refused.empty()) {
                                        const auto start = nextOf(page), revision = revisionOf(page);
                                        refused = codeOf([&] {
                                            page = bench.children(id, "", revision, start, 1000);
                                            return 0;
                                        });
                                    }
                                    then("children is refused once it is full, and nothing was changed by the refusal", [&] {
                                        requireEqual(refused, std::string{code::limit});
                                        requireEqual(bench.memory(id)->size(), inspect::maxMemoryNodes);
                                        requireEqual(static_cast<double>(bench.memory(id)->revision()), revisionOf(page));
                                    });
                                    then("folding makes room again", [&] {
                                        const auto folded = bench.fold(id, "", revisionOf(page));
                                        require(!resyncOf(folded));
                                        requireEqual(bench.memory(id)->size(), std::size_t{1});
                                        requireEqual(nodesOf(bench.children(id, "", revisionOf(folded), 20'000, 10)).size(), std::size_t{11});
                                    });
                                }};

const Scenario watchFirstOpening{
    "The root is always unfoldable, and the other calls need it in the memory (ADR-027, section 3)",
    {"inspector"},
    [] {
        Bench                                 bench;
        std::vector<std::vector<std::string>> rows{
            {"a", "b"},
            {"c"}
        };
        const auto id = bench.watch("rows", visiting(rows));
        then("a watch found by watches() is opened by children of the root, on an empty memory", [&] {
            const auto listed = bench.registry->page(0, 0, 10, 4096);
            requireEqual(std::get<0>(std::get<0>(listed).front()), static_cast<double>(id));
            requireEqual(std::get<2>(std::get<0>(listed).front()), std::string{"sequence"});
            require(std::get<4>(std::get<0>(listed).front()));
            const auto revision = bench.reset(id);
            // Nothing is in the memory yet: the root itself is refused to value, fold and write.
            checkEqual(codeOf([&] {
                           return bench.value(id, "");
                       }),
                       std::string{code::unknown});
            checkEqual(codeOf([&] {
                           return bench.fold(id, "", revision);
                       }),
                       std::string{code::unknown});
            checkEqual(codeOf([&] {
                           return bench.write(id, revision, "", "", "x");
                       }),
                       std::string{code::unknown});
            checkEqual(codeOf([&] {
                           return bench.children(id, "/#0", revision);
                       }),
                       std::string{code::unknown});
            const auto page = bench.children(id, "", revision);
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/#0", "/#1"});
            require(revisionOf(page) != revision);
            require(bench.memory(id)->contains("") && bench.memory(id)->contains("/#1"));
            requireEqual(bench.memory(id)->size(), std::size_t{3});
        });
        then("a first call with a revision the memory does not have is answered by a resynchronization, whose revision opens the root", [&] {
            const auto first = bench.children(id, "", 0);
            require(resyncOf(first) && nodesOf(first).empty());
            const auto page = bench.children(id, "", revisionOf(first));
            require(!resyncOf(page));
            requireEqual(nodesOf(page).size(), std::size_t{3});
        });
        then("after reset the tree is built again from the root, and the unfolded paths are found again", [&] {
            auto page = bench.open(id);
            page      = bench.children(id, "/#0", revisionOf(page));
            requireEqual(bench.memory(id)->size(), std::size_t{5});
            const auto revision = bench.reset(id);
            requireEqual(bench.memory(id)->size(), std::size_t{0});
            checkEqual(codeOf([&] {
                           return bench.children(id, "/#0", revision);
                       }),
                       std::string{code::unknown});
            page = bench.children(id, "", revision);
            require(hasNode(page, "/#0"));
            page = bench.children(id, "/#0", revisionOf(page));
            requireEqual(pathsOf(page), std::vector<std::string>{"/#0", "/#0/#0", "/#0/#1"});
        });
        then("fold forgets a node's descendants and gives a new revision; the root's keeps the root", [&] {
            auto page = bench.open(id);
            page      = bench.children(id, "/#0", revisionOf(page));
            page      = bench.children(id, "/#1", revisionOf(page));
            requireEqual(bench.memory(id)->size(), std::size_t{6});
            auto folded = bench.fold(id, "/#0", revisionOf(page));
            require(nodesOf(folded).empty() && !resyncOf(folded) && revisionOf(folded) != revisionOf(page));
            requireEqual(bench.memory(id)->size(), std::size_t{4});
            require(bench.memory(id)->contains("/#0") && !bench.memory(id)->contains("/#0/#0"));
            folded = bench.fold(id, "", revisionOf(folded));
            requireEqual(bench.memory(id)->size(), std::size_t{1});
            require(bench.memory(id)->contains(""));
            // An older revision folds nothing: the memory is emptied and the inspector reads again.
            page             = bench.children(id, "", revisionOf(folded));
            const auto stale = bench.fold(id, "", revisionOf(folded));
            require(resyncOf(stale));
            requireEqual(bench.memory(id)->size(), std::size_t{0});
        });
        then("a scalar root gives a page of itself alone", [&] {
            int        answer = 42;
            const auto scalar = bench.watch("answer", visiting(answer));
            const auto page   = bench.open(scalar);
            requireEqual(nodesOf(page).size(), std::size_t{1});
            requireEqual(valueOf(nodesOf(page).front()), std::string{"42"});
            requireEqual(flagsOf(nodesOf(page).front()), flag::writable);
            requireEqual(nextOf(page), 0.0);
        });
        then("a node that is gone from the object is told removed, and leaves the memory with its descendants", [&] {
            auto page = bench.open(id);
            page      = bench.children(id, "/#1", revisionOf(page));
            rows.pop_back();
            page = bench.children(id, "/#1", revisionOf(page));
            requireEqual(nodesOf(page).size(), std::size_t{1});
            requireEqual(flagsOf(nodesOf(page).front()), flag::removed);
            require(!bench.memory(id)->contains("/#1") && !bench.memory(id)->contains("/#1/#0"));
        });
    }};

const Scenario watchListsStayCoherent{"A list of children is read on one state, or starts again (ADR-027, section 3, \"Cohérence des listes d'enfants\")",
                                      {"inspector"},
                                      [] {
                                          Bench bench;
                                          then("an insertion at the head between two pages makes the list start again, and its last element is read", [&] {
                                              std::vector<std::string> letters{"a", "b", "c", "d"};
                                              const auto               id   = bench.watch("letters", visiting(letters));
                                              auto                     page = bench.open(id, 2);
                                              requireEqual(valuesOf(page), std::vector<std::string>{"", "a", "b"});
                                              requireEqual(nextOf(page), 2.0);
                                              letters.insert(letters.begin(), "x");
                                              page = bench.children(id, "", revisionOf(page), 2, 2);
                                              requireEqual(nodesOf(page).size(), std::size_t{1});
                                              requireEqual(flagsOf(nodesOf(page).front()), flag::childrenChanged);
                                              requireEqual(countOf(nodesOf(page).front()), std::size_t{5});
                                              requireEqual(nextOf(page), 0.0);
                                              requireEqual(bench.memory(id)->size(), std::size_t{1});
                                              std::vector<std::string> read;
                                              double                   start = 0;
                                              do {
                                                  page = bench.children(id, "", revisionOf(page), start, 2);
                                                  requireEqual(pathOf(nodesOf(page).front()), std::string{});
                                                  for (std::size_t index = 1; index < nodesOf(page).size(); ++index)
                                                      read.push_back(valueOf(nodesOf(page)[index]));
                                                  start = nextOf(page);
                                              } while (start != 0);
                                              requireEqual(read, std::vector<std::string>{"x", "a", "b", "c", "d"});
                                          });
                                          then("with keys, a later page that brings back a key already served at another position starts the list again", [&] {
                                              Keyed root;
                                              for (const auto* name : {"a", "b", "c", "d"})
                                                  root.items.push_back(Keyed{.name = name, .text = name, .items = {}, .keyFails = false, .visits = 0});
                                              const auto id   = bench.watch("swapped", visiting(root));
                                              auto       page = bench.open(id, 2);
                                              requireEqual(pathsOf(page), std::vector<std::string>{"", "/ka", "/kb"});
                                              std::swap(root.items[0], root.items[2]);  // c, b, a, d: the size and the place of b are kept
                                              page = bench.children(id, "", revisionOf(page), 2, 2);
                                              requireEqual(nodesOf(page).size(), std::size_t{1});
                                              requireEqual(flagsOf(nodesOf(page).front()), flag::childrenChanged);
                                              page = bench.children(id, "", revisionOf(page), 0, 10);
                                              requireEqual(pathsOf(page), std::vector<std::string>{"", "/kc", "/kb", "/ka", "/kd"});
                                          });
                                          then("with keys, an insertion that keeps the number of children is seen by the key of the last child served", [&] {
                                              Keyed root;
                                              for (const auto* name : {"a", "b", "c", "d"})
                                                  root.items.push_back(Keyed{.name = name, .text = name, .items = {}, .keyFails = false, .visits = 0});
                                              const auto id   = bench.watch("keyed", visiting(root));
                                              auto       page = bench.open(id, 2);
                                              requireEqual(pathsOf(page), std::vector<std::string>{"", "/ka", "/kb"});
                                              root.items.insert(root.items.begin(),
                                                                Keyed{.name = "x", .text = "x", .items = {}, .keyFails = false, .visits = 0});
                                              root.items.pop_back();  // still four children: only the key of "b", now third, tells the change
                                              page = bench.children(id, "", revisionOf(page), 2, 2);
                                              requireEqual(nodesOf(page).size(), std::size_t{1});
                                              requireEqual(flagsOf(nodesOf(page).front()), flag::childrenChanged);
                                              page = bench.children(id, "", revisionOf(page), 0, 10);
                                              requireEqual(pathsOf(page), std::vector<std::string>{"", "/kx", "/ka", "/kb", "/kc"});
                                          });
                                      }};

const Scenario watchKeys{
    "A paged source's keys name its elements in the paths, within what can be checked locally (ADR-027, section 5)",
    {"inspector"},
    [] {
        Bench bench;
        Keyed root;
        then("a key with '/', '%' and bytes that are not ASCII is carried by the path, and found again", [&] {
            root.items.push_back(Keyed{.name     = "a/b%\xC3\xA9",
                                       .text     = "odd",
                                       .items    = {Keyed{.name = "in", .text = "inner", .items = {}, .keyFails = false, .visits = 0}},
                                       .keyFails = false,
                                       .visits   = 0});
            const auto id   = bench.watch("keyed", visiting(root));
            auto       page = bench.open(id);
            requireEqual(pathOf(nodesOf(page)[1]), std::string{"/ka%2Fb%25%C3%A9"});
            requireEqual(labelOf(nodesOf(page)[1]), std::string{"a/b%\xC3\xA9"});
            page = bench.children(id, pathOf(nodesOf(page)[1]), revisionOf(page));
            requireEqual(pathsOf(page), std::vector<std::string>{"/ka%2Fb%25%C3%A9", "/ka%2Fb%25%C3%A9/kin"});
        });
        then("a key whose encoding passes 128 bytes, or that fails, tells its element by position, not writable", [&] {
            root.items.push_back(Keyed{.name = std::string(inspect::maxKeyBytes + 1, 'k'), .text = "long", .items = {}, .keyFails = false, .visits = 0});
            root.items.push_back(
                Keyed{.name = std::string(43, '/'), .text = "encoded long", .items = {}, .keyFails = false, .visits = 0});  // 129 bytes once encoded
            root.items.push_back(Keyed{.name = "short", .text = "short", .items = {}, .keyFails = false, .visits = 0});
            const auto id   = bench.watch("keyed", visiting(root));
            auto       page = bench.open(id);
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/#0", "/#1", "/kshort"});
            // The elements are sequences here: writability is read on a scalar member below.
            root.keyFails = true;
            page          = bench.children(id, "", revisionOf(page));
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/#0", "/#1", "/#2"});
            require(!bench.memory(id)->contains("/kshort"));
        });
        then("two elements of a page with one key are told by position, with one diagnostic", [&] {
            Rows rows{
                .rows = {{"a", 1}, {"dup", 2}, {"dup", 3}, {"b", 4}, {"dup", 5}}
            };
            const auto id   = bench.watch("rows", visiting(rows));
            auto       page = bench.open(id, 4);
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/ka", "/#1", "/#2", "/kb"});
            checkEqual(flagsOf(nodeAt(page, "/ka")), flag::writable);
            checkEqual(flagsOf(nodeAt(page, "/#1")), 0u);
            checkEqual(flagsOf(nodeAt(page, "/#2")), 0u);
            requireEqual(bench.diagnosticCount(), std::size_t{1});
            require(bench.lastDiagnostic().contains("'rows'"));
            page = bench.children(id, "", revisionOf(page), 0, 4);
            requireEqual(bench.diagnosticCount(), std::size_t{1});
            // A pass keeps them by position: their keys are not compared, so nothing is told changed.
            const auto pass = bench.changes(id, revisionOf(page));
            require(nodesOf(pass).empty());
            // The same key in another page escapes the check: its element takes the path, and the precondition of a write
            // then compares what is shown with what find() reaches.
            page = bench.children(id, "", revisionOf(pass), 4, 4);
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/kdup"});
            requireEqual(valueOf(nodesOf(page)[1]), std::string{"5"});
            const auto answer = bench.write(id, revisionOf(page), "/kdup", "5", "50");
            requireEqual(std::get<1>(answer), std::string{code::stale});
            requireEqual(rows.rows[1].value, 2);
            requireEqual(rows.rows[4].value, 5);
        });
        then("under an element told by its position, every descendant is read-only, keyed or not", [&] {
            KeyedItems items{
                .items = {{std::string(inspect::maxKeyBytes + 1, 'k'), {1, 2}}, {"short", {3}}}
            };
            const auto id   = bench.watch("items", visiting(items));
            auto       page = bench.open(id);
            requireEqual(pathsOf(page), std::vector<std::string>({"", "/#0", "/kshort"}));
            page = bench.children(id, "/#0", revisionOf(page));
            requireEqual(pathsOf(page), std::vector<std::string>({"/#0", "/#0/#0", "/#0/#1"}));
            checkEqual(flagsOf(nodesOf(page)[1]), 0u);
            checkEqual(flagsOf(nodesOf(page)[2]), 0u);
            requireEqual(codeOf([&] {
                             return bench.write(id, revisionOf(page), "/#0/#1", "2", "9");
                         }),
                         std::string{code::readOnly});
            // A branch whose whole path is keyed stays writable.
            page = bench.children(id, "/kshort", revisionOf(page));
            checkEqual(flagsOf(nodesOf(page)[1]), flag::writable);
        });
        then("a key that find() resolves to an element with another key is a node removed", [&] {
            struct Lying : Keyed {
                [[nodiscard]] std::optional<std::size_t> find(std::string_view) const {
                    return 0;  // always the first element, whatever the key
                }
            };
            Lying lying;
            lying.items.push_back(Keyed{.name = "a", .text = "a", .items = {Keyed{.name = "x", .text = {}, .items = {}}}});
            lying.items.push_back(Keyed{.name = "b", .text = "b", .items = {Keyed{.name = "y", .text = {}, .items = {}}}});
            const auto id   = bench.watch("lying", visiting(lying));
            auto       page = bench.open(id);
            requireEqual(pathsOf(page), std::vector<std::string>{"", "/ka", "/kb"});
            page = bench.children(id, "/kb", revisionOf(page));
            requireEqual(nodesOf(page).size(), std::size_t{1});
            requireEqual(flagsOf(nodesOf(page).front()), flag::removed);
            require(!bench.memory(id)->contains("/kb"));
        });
    }};

const Scenario watchFullValues{"A full value is read by fragments of an identified snapshot (ADR-027, section 3, \"Valeur complète\")", {"inspector"}, [] {
                                   Bench bench;
                                   bench.limits.responseBudget = inspect::limitsOf<Small>().responseBudget;
                                   std::vector<std::string> texts{std::string{}, "second"};
                                   for (int index = 0; index < 20'000; ++index)
                                       texts[0] += "h\xC3\xA9\xE2\x82\xAC";  // 1, 2 and 3 bytes: fragments must not cut them
                                   const auto id   = bench.watch("texts", visiting(texts));
                                   const auto page = bench.open(id);
                                   const auto read = [&](std::string_view path) {
                                       std::string whole;
                                       auto        answer = bench.value(id, path);
                                       while (true) {
                                           const auto& [part, total, next, capped, snapshot] = answer;
                                           require(part.size() <= bench.limits.responseBudget - inspect::responseHeaderBytes);
                                           require(part.empty() || (static_cast<unsigned char>(part.front()) & 0xC0u) != 0x80u);
                                           whole += part;
                                           if (next == 0)
                                               return std::pair{whole, capped};
                                           requireEqual(next, static_cast<double>(whole.size()));
                                           answer = bench.value(id, path, snapshot, next);
                                       }
                                   };
                                   then("a value larger than a message is read whole, never cutting a character", [&] {
                                       require(texts[0].size() > http::maxMessageSize<Small>());
                                       require((flagsOf(nodeAt(page, "/#0")) & flag::truncated) != 0);
                                       require(valueOf(nodeAt(page, "/#0")).size() <= inspect::maxTextBytes);
                                       const auto [whole, capped] = read("/#0");
                                       require(whole == texts[0]);
                                       require(!capped);
                                       require(!bench.session->hasSnapshot());  // freed with its last fragment
                                       requireEqual(read("/#1").first, std::string{"second"});
                                   });
                                   then("the snapshot stays what it was when the object changes between two fragments", [&] {
                                       const auto expected = texts[0];
                                       auto       answer   = bench.value(id, "/#0");
                                       texts[0]            = "changed";
                                       std::string whole   = std::get<0>(answer);
                                       while (std::get<2>(answer) != 0) {
                                           answer = bench.value(id, "/#0", std::get<4>(answer), std::get<2>(answer));
                                           whole += std::get<0>(answer);
                                       }
                                       require(whole == expected);
                                   });
                                   then("a snapshot is cut at its cap, which the answer tells", [&] {
                                       bench.limits.maxSnapshotBytes = 10'000;
                                       const auto [whole, capped]    = read("/#0");
                                       require(capped);
                                       require(whole.size() <= 10'000 && whole.size() > 9'990);
                                       require(texts[0].starts_with(whole));
                                   });
                                   then("a second reading replaces the snapshot, and the rest of the first one is refused", [&] {
                                       const auto first  = bench.value(id, "/#0");
                                       const auto second = bench.value(id, "/#0");
                                       require(std::get<4>(first) != std::get<4>(second));
                                       requireEqual(codeOf([&] {
                                                        return bench.value(id, "/#0", std::get<4>(first), std::get<2>(first));
                                                    }),
                                                    std::string{code::stale});
                                       // The second reading goes on; an offset it was not given, another path or a snapshot of 0 with an offset do not.
                                       checkEqual(codeOf([&] {
                                                      return bench.value(id, "/#0", std::get<4>(second), std::get<2>(second) - 1);
                                                  }),
                                                  std::string{code::stale});
                                       checkEqual(codeOf([&] {
                                                      return bench.value(id, "/#1", std::get<4>(second), std::get<2>(second));
                                                  }),
                                                  std::string{code::stale});
                                       checkEqual(codeOf([&] {
                                                      return bench.value(id, "/#0", 0, 12);
                                                  }),
                                                  std::string{code::stale});
                                       require(!std::get<0>(bench.value(id, "/#0", std::get<4>(second), std::get<2>(second))).empty());
                                   });
                                   then("a node that left the object, or a path the memory does not hold, has no value", [&] {
                                       checkEqual(codeOf([&] {
                                                      return bench.value(id, "/#5");
                                                  }),
                                                  std::string{code::unknown});
                                       texts.pop_back();
                                       checkEqual(codeOf([&] {
                                                      return bench.value(id, "/#1");
                                                  }),
                                                  std::string{code::stale});
                                   });
                               }};

const Scenario watchBoundedFormatting{"A value is written into a bounded receiver, never formatted whole then cut (ADR-027, section 3, \"Formatage borné\")",
                                      {"inspector"},
                                      [] {
                                          Bench bench;
                                          then("a string of 100 MiB is copied up to the bound only", [&] {
                                              std::vector<std::string> texts{std::string(100u * 1024 * 1024, 's')};
                                              const auto               id   = bench.watch("texts", visiting(texts));
                                              const auto               page = bench.open(id);
                                              const auto&              node = nodesOf(page)[1];
                                              require(valueOf(node).size() <= inspect::maxTextBytes);
                                              require(valueOf(node).ends_with("\xE2\x80\xA6"));
                                              requireEqual(flagsOf(node), flag::truncated);  // cut, hence not writable
                                              const auto answer = bench.value(id, "/#0");
                                              requireEqual(std::get<1>(answer), static_cast<double>(inspect::maxSnapshotBytes));
                                              require(std::get<3>(answer));
                                          });
                                          then("a formatter that writes more than 1 GiB to the output iterator is cut, and the cut is told", [&] {
                                              std::vector<Wide> wides{
                                                  Wide{.size = (std::size_t{1} << 30) + 5, .failing = false},
                                                  Wide{                         .size = 3, .failing = false}
                                              };
                                              const auto id   = bench.watch("wides", visiting(wides));
                                              const auto page = bench.open(id);
                                              requireEqual(kindOf(nodesOf(page)[1]), std::string{"formatted"});
                                              require(valueOf(nodesOf(page)[1]).size() <= inspect::maxTextBytes);
                                              requireEqual(flagsOf(nodesOf(page)[1]), flag::truncated);
                                              requireEqual(valueOf(nodesOf(page)[2]), std::string{"www"});
                                              requireEqual(flagsOf(nodesOf(page)[2]), 0u);
                                          });
                                          then("the receiver stores no more than its bound, and knows what it lost", [&] {
                                              inspect::TextSink sink{8};
                                              sink.append(std::string_view{"abc"});
                                              sink.format(123456789);
                                              requireEqual(sink.text(), std::string{"abc12345"});
                                              require(sink.truncated() && sink.full());
                                              inspect::TextSink exact{4};
                                              exact.format(1234);
                                              require(!exact.truncated());
                                          });
                                          then("an exception of a formatter becomes the value of its node, which is opaque", [&] {
                                              std::vector<Wide> wides{
                                                  Wide{.size = 1,  .failing = true},
                                                  Wide{.size = 2, .failing = false}
                                              };
                                              const auto id   = bench.watch("wides", visiting(wides));
                                              const auto page = bench.open(id);
                                              requireEqual(kindOf(nodesOf(page)[1]), std::string{"opaque"});
                                              requireEqual(valueOf(nodesOf(page)[1]),
                                                           std::string{"\xE2\x80\xB9"
                                                                       "exception : formatter\xE2\x90\x8A"
                                                                       "failed\xE2\x80\xBA"});
                                              requireEqual(valueOf(nodesOf(page)[2]), std::string{"ww"});
                                          });
                                      }};
}  // namespace watch_tree
