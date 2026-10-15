/// @brief The wf tool manager: web tools installed into a WebFront application while it is
/// developed, stored by fs::ToolStore and served by fs::PackageFS (see docs/UI_COMPONENTS.md).
#pragma once

#include "../system/PackageFS.hpp"
#include "../system/ToolStore.hpp"
#include "Terminal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront {

/// Application type serving the terminal page and the installed tools next to the default assets.
using ToolsApp = BasicWF<NetProvider, fs::DefaultFSWith<fs::TerminalFS, fs::PackageFS>>;

struct ToolManagerOptions {
    /// npm registry that 'wf search' and 'wf install' query from the browser: an absolute URL, or a
    /// path on the application's origin (used by the tests' local registry).
    std::string registry = "https://registry.npmjs.org";
};

namespace tools::detail {

// JavaScript numbers are exact integers up to 2^53.
inline std::uint64_t toIndex(double value) {
    const bool integral = std::floor(value) == value;  // false for NaN
    const bool inRange  = value >= 0 && value <= 9007199254740992.0;
    if (!integral || !inRange)
        throw std::invalid_argument("Invalid file offset or list index");
    return static_cast<std::uint64_t>(value);
}

/// Bridge strings carry a 16-bit length; a list page stays below it, like terminal output.
inline constexpr std::size_t maxListBytes = 60000;

/// The lines of the installed tools from index 'first', as many whole lines as fit in
/// maxListBytes; empty past the last tool. A line always fits, since names, versions, paths and
/// texts are bounded by the store's validation.
inline std::string listPage(const fs::ToolStore& store, std::uint64_t first) {
    const auto  installed = store.installed();
    std::string page;
    auto        tool = installed.begin();
    std::advance(tool, static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(first, installed.size())));
    for (; tool != installed.end(); ++tool) {
        const auto& [name, record] = *tool;
        const auto line            = name + "\t" + record.version + "\t" + fs::PackageFS::url(name, record.entry) + "\t" + record.title + "\n";
        if (page.size() + line.size() > maxListBytes)
            break;
        page += line;
    }
    return page;
}

inline void requireArguments(CommandArgs args, std::size_t count) {
    if (args.size() != count)
        throw std::invalid_argument("Wrong number of arguments. Type 'wf help'.");
}

inline void usage(std::ostream& output) {
    output << "wf search [<term>...]               Search the registry for tools\n"
              "wf install <package>[@<version>]  Download, verify and install a tool\n"
              "wf list                           List the installed tools\n"
              "wf open <package>                 Open a tool in a new tab or window\n"
              "wf remove <package>               Uninstall a tool";
}

/// Splits "name[@version]"; a scoped name starts with '@'. The name is validated, the version is
/// left to the registry since it may be a tag such as "latest".
inline std::pair<std::string, std::string> splitSpec(std::string_view spec) {
    const auto at             = spec.find('@', 1);
    auto       name           = std::string(spec.substr(0, at));
    auto       packageVersion = at == std::string_view::npos ? std::string{} : std::string(spec.substr(at + 1));
    fs::tools::validatePackageName(name);
    if (at != std::string_view::npos && packageVersion.empty())
        throw std::invalid_argument("Missing version after '@' in '" + std::string(spec) + "'");
    return {std::move(name), std::move(packageVersion)};
}

/// The browser part of 'wf search', 'wf install' and 'wf open' (src/components/tools/wf.mjs).
inline BrowserCommand inBrowser(std::vector<std::string> args) {
    return {std::string(fs::PackageFS::toolManagerModule), std::move(args)};
}

inline BrowserCommand searchTools(const std::string& registry, CommandArgs args) {
    std::string term;
    for (const auto& word : args.subspan(1))
        term += (term.empty() ? "" : " ") + word;
    return inBrowser({"search", registry, std::move(term)});
}

inline BrowserCommand installTool(const std::string& registry, CommandArgs args) {
    requireArguments(args, 2);
    auto [name, packageVersion] = splitSpec(args[1]);
    return inBrowser({"install", registry, std::move(name), std::move(packageVersion)});
}

inline BrowserCommand openTool(const fs::ToolStore& store, CommandArgs args) {
    requireArguments(args, 2);
    const auto record = store.find(args[1]);
    if (!record)
        throw std::invalid_argument(args[1] + " is not installed");
    return inBrowser({"open", fs::PackageFS::url(record->name, record->entry)});
}

inline void removeTool(fs::ToolStore& store, CommandArgs args, std::ostream& output) {
    requireArguments(args, 2);
    if (!store.remove(args[1]))
        throw std::invalid_argument(args[1] + " is not installed");
    output << "Removed " << args[1] << ".";
}

inline void listTools(const fs::ToolStore& store, CommandArgs args, std::ostream& output) {
    requireArguments(args, 1);
    const auto installed = store.installed();
    if (installed.empty()) {
        output << "No tool installed.";
        return;
    }
    std::size_t width = 0;
    for (const auto& [name, record] : installed)
        width = std::max(width, name.size() + 1 + record.version.size());
    const char* separator = "";
    for (const auto& [name, record] : installed) {
        const auto label = name + "@" + record.version;
        output << separator << label << std::string(width - label.size() + 2, ' ') << record.title;
        separator = "\n";
    }
}

inline std::optional<BrowserCommand> runWf(fs::ToolStore& store, const std::string& registry, CommandArgs args, std::ostream& output) {
    const std::string_view command = args.empty() ? "help" : std::string_view(args[0]);
    if (command == "search")
        return searchTools(registry, args);
    if (command == "install")
        return installTool(registry, args);
    if (command == "open")
        return openTool(store, args);
    if (command == "help")
        usage(output);
    else if (command == "list")
        listTools(store, args, output);
    else if (command == "remove")
        removeTool(store, args, output);
    else
        throw std::invalid_argument("Unknown wf command '" + std::string(command) + "'. Type 'wf help'.");
    return std::nullopt;
}

}  // namespace tools::detail

/// Adds the 'wf' command to a terminal and the bridge functions the page uses to install tools.
/// 'wf search', 'wf install' and 'wf open' continue in the browser module
/// /webfront/components/tools/wf.mjs, which PackageFS serves, and which downloads from
/// options.registry. It calls:
///   webfront.pkg.begin(name, version)
///   webfront.pkg.writeFile(name, version, path, offset, Uint8Array)
///   webfront.pkg.commit(name, version, integrity, entry, title)
///   webfront.pkg.list(first) -> "name\tversion\tentry URL\ttitle\n" lines of the installed tools
///                               from index 'first', paged below the bridge's string limit: the
///                               page calls it again from the next index until it gets ""
/// The tools live in Location::get(), which must match the application's fs::BasicPackageFS
/// (ToolsApp uses fs::CurrentDirectory). The registered functions share ownership of the store, so
/// a ToolManager may be destroyed before the application.
///
/// Development only: an installed tool runs in the application's origin and can call every
/// registered C++ function, so the trust model is that of 'npm install'.
template <typename App, typename Location = fs::CurrentDirectory>
class ToolManager {
public:
    using PackageFS = fs::BasicPackageFS<Location>;

    ToolManager(App& app, Terminal<App>& terminal, ToolManagerOptions options = {}) : toolStore(std::make_shared<fs::ToolStore>(Location::get())) {
        app.template cppFunction<void, std::string, std::string>("webfront.pkg.begin",
                                                                 [registry = toolStore](const std::string& name, const std::string& packageVersion) {
                                                                     registry->begin(name, packageVersion);
                                                                 });
        app.template cppFunction<void, std::string, std::string, std::string, double, std::vector<std::uint8_t>>(
            "webfront.pkg.writeFile",
            [registry = toolStore](const std::string&               name,
                                   const std::string&               packageVersion,
                                   const std::string&               path,
                                   double                           offset,
                                   const std::vector<std::uint8_t>& bytes) {
                registry->write(name, packageVersion, {.path = path, .offset = tools::detail::toIndex(offset), .bytes = bytes});
            });
        app.template cppFunction<void, std::string, std::string, std::string, std::string, std::string>(
            "webfront.pkg.commit",
            [registry = toolStore](const std::string& name,
                                   const std::string& packageVersion,
                                   const std::string& integrity,
                                   const std::string& entry,
                                   const std::string& title) {
                registry->commit({.name = name, .version = packageVersion, .integrity = integrity, .entry = entry, .title = title});
            });
        app.template cppFunction<std::string, double>("webfront.pkg.list", [registry = toolStore](double first) {
            return tools::detail::listPage(*registry, tools::detail::toIndex(first));
        });
        terminal.command("wf",
                         "Manage web tools: wf search | install | list | open | remove",
                         [registry = toolStore, npm = std::move(options.registry)](CommandArgs args, std::ostream& output) {
                             return tools::detail::runWf(*registry, npm, args, output);
                         });
    }

    [[nodiscard]] fs::ToolStore& store() {
        return *toolStore;
    }

private:
    std::shared_ptr<fs::ToolStore> toolStore;
};

template <typename App>
ToolManager(App&, Terminal<App>&, ToolManagerOptions = {}) -> ToolManager<App>;

}  // namespace webfront
