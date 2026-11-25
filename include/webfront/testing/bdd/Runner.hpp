// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — testing
// Role: run(), the runner of webfront::bdd: options, filters, order, shards, scenario passes, summary and JUnit report.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public maintained interface (docs/BDD.md); included by the one file that defines main(), not by every test file

// The runner of the registered scenarios. Only the file that defines main() needs it:
// @code
// #include <webfront/testing/bdd/Runner.hpp>
// int main(int argc, char** argv) { return webfront::bdd::run(argc, argv); }
// @endcode

#pragma once

#include <webfront/testing/bdd/Core.hpp>

#include <cstdio>
#include <ctime>

namespace webfront::bdd {

namespace impl {

inline double now() {
    std::timespec time{};
    if (std::timespec_get(&time, TIME_UTC) != TIME_UTC)
        return 0.;
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

struct Options {
    std::vector<std::string_view> filters;
    std::string_view              exact;
    std::string_view              junit;
    bool                          list        = false;
    bool                          randomOrder = true;
    std::uint64_t                 seed        = 0;
    std::uint64_t                 shardCount  = 1;
    std::uint64_t                 shardIndex  = 0;
};

enum class Outcome { passed, failed, skipped };

struct Result {
    const Entry* entry;
    Outcome      outcome;
    double       seconds;
    std::string  report;
};

/// Whether a scenario matches a filter: "[a][b]" requires every tag, otherwise the title, with or without "Scenario: ",
/// may start or end with a '*' wildcard.
inline bool matches(const Entry& entry, std::string_view filter) {
    if (filter.starts_with('[')) {
        while (filter.starts_with('[')) {
            auto end = filter.find(']');
            if (end == std::string_view::npos)
                return false;
            auto tag   = filter.substr(1, end - 1);
            bool found = false;
            for (const auto& entryTag : entry.tags)
                found = found || entryTag == tag;
            if (!found)
                return false;
            filter.remove_prefix(end + 1);
        }
        return filter.empty();
    }
    if (filter.starts_with("Scenario: "))
        filter.remove_prefix(10);
    std::string_view title  = entry.title;
    bool             prefix = filter.ends_with('*'), suffix = filter.starts_with('*');
    if (prefix)
        filter.remove_suffix(1);
    if (suffix && !filter.empty())
        filter.remove_prefix(1);
    if (prefix && suffix)
        return title.find(filter) != std::string_view::npos;
    if (prefix)
        return title.starts_with(filter);
    if (suffix)
        return title.ends_with(filter);
    return title == filter;
}

/// Whether a scenario is selected: each argument is a comma-separated list of alternatives.
inline bool selected(const Entry& entry, const Options& options) {
    if (!options.exact.empty())
        return entry.title == options.exact;
    if (options.filters.empty())
        return true;
    for (auto rest : options.filters)
        while (true) {
            auto comma = rest.find(',');
            if (matches(entry, rest.substr(0, comma)))
                return true;
            if (comma == std::string_view::npos)
                break;
            rest.remove_prefix(comma + 1);
        }
    return false;
}

/// Runs every pass of a scenario. A scenario-starting listener that throws fails the scenario, which does not run, and
/// the run goes on, so that the run-ending listeners are still called.
inline Result runScenario(const Entry& entry) {
    auto& current = state();
    Node  root;
    current.title = entry.title;
    current.report.clear();
    current.path.clear();
    auto failuresBefore = current.failures.load();
    bool skipped        = false;
    bool started        = true;
    auto start          = now();
    for (auto listener : listeners().scenarioStarting)
        try {
            listener();
        } catch (...) {
            reportFailure(std::source_location{}, concat("a scenario-starting listener threw: ", exceptionMessage(std::current_exception())));
            started = false;
            break;
        }
    for (current.pass = 1; started; ++current.pass) {
        current.current   = &root;
        current.unwinding = false;
        current.path.clear();
        bool aborted = false;
        try {
            entry.body();
        } catch (const AssertionFailure&) {
            aborted = !current.unwinding;
        } catch (const Skipped&) {
            skipped = true;
        } catch (...) {
            reportFailure(std::source_location{}, concat("unexpected exception: ", exceptionMessage(std::current_exception())));
            aborted = true;
        }
        root.complete = root.childrenComplete();
        // A failure outside any section, a skip, a pass that entered no section or the end of every branch ends the
        // scenario. After a failure in a section, another pass looks for the sections that follow it.
        if (aborted || skipped || root.enteredChildPass != current.pass || (root.complete && !current.unwinding))
            break;
    }
    auto outcome = current.failures.load() != failuresBefore ? Outcome::failed : skipped ? Outcome::skipped : Outcome::passed;
    return {&entry, outcome, now() - start, current.report};
}

/// The exceptions that run-starting or run-ending listeners threw: the name of the JUnit test case that reports them,
/// and their messages, one per line.
struct RunError {
    std::string_view name;
    std::string_view what;
    std::string      messages;
};

/// Calls run listeners and returns the messages of the exceptions they threw, one per line, and the reports of the
/// failures they recorded (check()), all of which are also printed; empty when every listener returned without a
/// failure. The run-starting listeners stop at the first exception, since the run does not go on; every run-ending
/// listener is called.
inline std::string callRunListeners(const std::vector<void (*)()>& listeners, std::string_view name, std::string_view what, bool stopAtFirst) {
    auto& current = state();
    current.title = name;
    current.path.clear();
    current.report.clear();
    std::string errors;
    for (auto listener : listeners)
        try {
            listener();
        } catch (const AssertionFailure&) {
            if (stopAtFirst)  // require() recorded the failure
                break;
        } catch (...) {
            auto message = exceptionMessage(std::current_exception());
            errors      += concat(message, '\n');
            print(concat("FAILED: ", what, ": ", message, '\n'));
            if (stopAtFirst)
                break;
        }
    return errors + current.report;
}

/// The length of the UTF-8 sequence that starts a text and that XML 1.0 accepts, or 0.
inline std::size_t utf8Length(std::string_view text) {
    auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    auto          lead = byte(0);
    std::size_t   length;
    std::uint32_t code;
    if (lead < 0x80)
        return 1;
    if (lead >= 0xc2 && lead <= 0xdf)
        length = 2, code = lead & 0x1fu;
    else if (lead >= 0xe0 && lead <= 0xef)
        length = 3, code = lead & 0x0fu;
    else if (lead >= 0xf0 && lead <= 0xf4)
        length = 4, code = lead & 0x07u;
    else
        return 0;
    if (text.size() < length)
        return 0;
    for (std::size_t index = 1; index != length; ++index) {
        if ((byte(index) & 0xc0) != 0x80)
            return 0;
        code = (code << 6) | (byte(index) & 0x3fu);
    }
    bool overlong = (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);
    bool excluded = (code >= 0xd800 && code <= 0xdfff) || code == 0xfffe || code == 0xffff || code > 0x10ffff;
    return overlong || excluded ? 0 : length;
}

/// Escapes a text for XML 1.0: the control characters it forbids, other than tab, line feed and carriage return, and
/// the bytes that are not valid UTF-8 are written as \xNN.
inline std::string xmlEscape(std::string_view text) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                escaped;
    while (!text.empty()) {
        auto character = static_cast<unsigned char>(text.front());
        auto length    = utf8Length(text);
        if (character == '&')
            escaped += "&amp;";
        else if (character == '<')
            escaped += "&lt;";
        else if (character == '>')
            escaped += "&gt;";
        else if (character == '"')
            escaped += "&quot;";
        else if (length == 0 || (character < 0x20 && character != '\t' && character != '\n' && character != '\r')) {
            escaped += "\\x";
            escaped += digits[character >> 4];
            escaped += digits[character & 0xf];
            length   = 1;
        } else
            escaped += text.substr(0, length);
        text.remove_prefix(length);
    }
    return escaped;
}

/// Writes the JUnit report. The exceptions of the run listeners, when there are any, are the error of one more test
/// case ("Run starting", "Run ending"), so that the tools that read the report see why the run failed.
inline bool writeJUnit(std::string_view path, const std::vector<Result>& results, double total, const std::vector<RunError>& runErrors = {}) {
    std::size_t failed = 0, skipped = 0, errors = runErrors.size();
    for (const auto& result : results) {
        failed  += result.outcome == Outcome::failed;
        skipped += result.outcome == Outcome::skipped;
    }
    auto xml = concat("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n  <testsuite name=\"tests\" tests=\"",
                      results.size() + errors,
                      "\" failures=\"",
                      failed,
                      "\" errors=\"",
                      errors,
                      "\" skipped=\"",
                      skipped,
                      "\" time=\"",
                      seconds(total),
                      "\">\n");
    for (const auto& result : results) {
        xml += concat("    <testcase classname=\"tests\" name=\"Scenario: ", xmlEscape(result.entry->title), "\" time=\"", seconds(result.seconds), '"');
        if (result.outcome == Outcome::passed)
            xml += "/>\n";
        else if (result.outcome == Outcome::failed)
            xml += concat(">\n      <failure message=\"failed\">", xmlEscape(result.report), "</failure>\n    </testcase>\n");
        else
            xml += ">\n      <skipped/>\n    </testcase>\n";
    }
    for (const auto& error : runErrors)
        xml += concat("    <testcase classname=\"tests\" name=\"",
                      error.name,
                      "\" time=\"0.000\">\n      <error message=\"",
                      error.what,
                      "\">",
                      xmlEscape(error.messages),
                      "</error>\n    </testcase>\n");
    xml += "  </testsuite>\n</testsuites>\n";
    std::string name{path};
    std::FILE*  file = nullptr;
#ifdef _WIN32
    // MSVC deprecates fopen.
    if (fopen_s(&file, name.c_str(), "wb") != 0)
        file = nullptr;
#else
    file = std::fopen(name.c_str(), "wb");
#endif
    if (!file)
        return false;
    bool written = std::fwrite(xml.data(), 1, xml.size(), file) == xml.size();
    return std::fclose(file) == 0 && written;
}

inline bool number(std::string_view text, std::uint64_t& value) {
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

/// Reads the options; returns an error, or "help" when the usage was printed.
inline std::string parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        std::string_view argument = argv[index];
        std::string_view value;
        bool takesValue = argument == "--exact" || argument == "--junit" || argument == "--order" || argument == "--rng-seed" || argument == "--shard-count"
                          || argument == "--shard-index";
        if (takesValue) {
            if (index + 1 >= argc)
                return concat(argument, " needs a value");
            value = argv[++index];
        }
        if (argument == "--list")
            options.list = true;
        else if (argument == "--exact")
            options.exact = value;
        else if (argument == "--junit")
            options.junit = value;
        else if (argument == "--order") {
            if (value != "decl" && value != "rand")
                return "--order is decl or rand";
            options.randomOrder = value == "rand";
        } else if (argument == "--rng-seed") {
            if (!number(value, options.seed))
                return "--rng-seed needs a number";
        } else if (argument == "--shard-count") {
            if (!number(value, options.shardCount))
                return "--shard-count needs a number";
        } else if (argument == "--shard-index") {
            if (!number(value, options.shardIndex))
                return "--shard-index needs a number";
        } else if (argument == "--help" || argument == "-h") {
            print(concat("usage: ",
                         argv[0],
                         " [filter...] [--exact <title>] [--list] [--order decl|rand] [--rng-seed <n>]\n"
                         "       [--shard-count <n> --shard-index <i>] [--junit <file>]\n"
                         "A filter is a title (\"Scenario: \" optional, '*' at either end) or tags \"[a][b]\"; commas separate "
                         "alternatives.\n"));
            return "help";
        } else if (argument.starts_with("--"))
            return concat("unknown option ", argument);
        else
            options.filters.push_back(argument);
    }
    if (options.shardCount == 0 || options.shardIndex >= options.shardCount)
        return "--shard-index must be lower than --shard-count";
    return {};
}

/// A small generator for the random order (splitmix64).
inline std::uint64_t nextRandom(std::uint64_t& seed) {
    std::uint64_t value = (seed += 0x9e3779b97f4a7c15u);
    value               = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9u;
    value               = (value ^ (value >> 27)) * 0x94d049bb133111ebu;
    return value ^ (value >> 31);
}

/// Why CTest could not name a scenario: its title is written in a bracket argument, one per line of `--list`.
inline std::string_view titleError(std::string_view title) {
    if (title.empty())
        return "is empty";
    if (title.find_first_of("\r\n") != std::string_view::npos)
        return "holds a line break";
    if (title.find("]==]") != std::string_view::npos)
        return "holds ]==]";
    return {};
}

}  // namespace impl

/// Runs the selected scenarios. Returns 0 when they pass, 1 when one fails or a run-ending listener throws, 2 for a usage
/// error or when nothing is selected, and 4 when every selected scenario was skipped.
inline int run(int argc, char** argv) {
    using namespace impl;
    Options options;
    options.seed = static_cast<std::uint64_t>(now() * 1e9);
    if (auto error = parse(argc, argv, options); !error.empty()) {
        if (error == "help")
            return 0;
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    std::vector<const Entry*> selection;
    const auto&               entries = registry();
    for (std::size_t index = 0; index != entries.size(); ++index) {
        if (auto error = titleError(entries[index].title); !error.empty()) {
            std::fprintf(stderr, "%s\n", concat(entries[index].file, ':', entries[index].line, ": the title of a scenario ", error).c_str());
            return 2;
        }
        for (std::size_t previous = 0; previous != index; ++previous)
            if (entries[previous].title == entries[index].title) {
                std::fprintf(stderr,
                             "%s\n",
                             concat(entries[index].file,
                                    ':',
                                    entries[index].line,
                                    ": the scenario \"",
                                    entries[index].title,
                                    "\" is already declared at ",
                                    entries[previous].file,
                                    ':',
                                    entries[previous].line)
                                 .c_str());
                return 2;
            }
        if (selected(entries[index], options))
            selection.push_back(&entries[index]);
    }
    if (options.list) {
        std::string titles;
        for (const auto* entry : selection)
            titles += concat(entry->title, '\n');
        print(titles);
        return 0;
    }
    if (options.shardCount > 1) {
        std::vector<const Entry*> shard;
        for (auto index = options.shardIndex; index < selection.size(); index += options.shardCount)
            shard.push_back(selection[static_cast<std::size_t>(index)]);
        selection = std::move(shard);
    }
    if (selection.empty()) {
        std::fprintf(stderr, "No scenario matches the filters\n");
        return options.shardCount > 1 ? 0 : 2;
    }
    if (options.randomOrder) {
        print(concat("Randomized with seed ", options.seed, '\n'));
        auto seed = options.seed;
        for (auto index = selection.size(); index > 1; --index)
            std::swap(selection[index - 1], selection[static_cast<std::size_t>(nextRandom(seed) % index)]);
    }

    // A run-starting listener that throws leaves the run without its preparation: no scenario runs, but the run-ending
    // listeners are still called, to finalize what the others prepared.
    std::vector<RunError> runErrors;
    std::vector<Result>   results;
    results.reserve(selection.size());
    auto start = now();
    if (auto errors = callRunListeners(listeners().runStarting, "Run starting", "a run-starting listener threw", true); !errors.empty())
        runErrors.push_back({"Run starting", "a run-starting listener failed", std::move(errors)});
    else
        for (const auto* entry : selection)
            results.push_back(runScenario(*entry));
    auto elapsed = now() - start;
    if (auto errors = callRunListeners(listeners().runEnding, "Run ending", "a run-ending listener threw", false); !errors.empty())
        runErrors.push_back({"Run ending", "a run-ending listener failed", std::move(errors)});
    bool ended = runErrors.empty();

    std::size_t failed = 0, skipped = 0;
    for (const auto& result : results) {
        failed  += result.outcome == Outcome::failed;
        skipped += result.outcome == Outcome::skipped;
    }
    print(concat(failed || !ended ? "FAILED" : "All scenarios passed",
                 " (",
                 state().assertions.load(),
                 " assertions in ",
                 results.size(),
                 " scenarios: ",
                 results.size() - failed - skipped,
                 " passed, ",
                 failed,
                 " failed, ",
                 skipped,
                 " skipped)\n"));
    if (!options.junit.empty() && !writeJUnit(options.junit, results, elapsed, runErrors)) {
        std::fprintf(stderr, "Cannot write %s\n", std::string{options.junit}.c_str());
        return 1;
    }
    if (failed || !ended)
        return 1;
    return skipped == results.size() ? 4 : 0;
}

}  // namespace webfront::bdd
