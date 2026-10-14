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
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace webfront {

/// Application type serving the terminal page and the installed tools next to the default assets.
using ToolsApp = BasicWF<NetProvider, fs::DefaultFSWith<fs::TerminalFS, fs::PackageFS>>;

namespace tools::detail {

// JavaScript numbers are exact integers up to 2^53.
inline std::uint64_t toOffset(double offset) {
    const bool integral = std::floor(offset) == offset;  // false for NaN
    if (!integral || offset < 0 || offset > 9007199254740992.0)
        throw std::invalid_argument("Invalid file offset");
    return static_cast<std::uint64_t>(offset);
}

inline void requireArguments(CommandArgs args, std::size_t count) {
    if (args.size() != count)
        throw std::invalid_argument("Wrong number of arguments. Type 'wf help'.");
}

inline void usage(std::ostream& output) {
    output << "wf list              List the installed tools\n"
              "wf remove <package>  Uninstall a tool";
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

inline void runWf(fs::ToolStore& store, CommandArgs args, std::ostream& output) {
    const std::string_view command = args.empty() ? "help" : std::string_view(args[0]);
    if (command == "help")
        return usage(output);
    if (command == "list")
        return listTools(store, args, output);
    if (command == "remove")
        return removeTool(store, args, output);
    throw std::invalid_argument("Unknown wf command '" + std::string(command) + "'. Type 'wf help'.");
}

}  // namespace tools::detail

/// Adds the 'wf' command to a terminal and the bridge functions the page uses to install tools:
///   webfront.pkg.begin(name, version)
///   webfront.pkg.writeFile(name, version, path, offset, Uint8Array)
///   webfront.pkg.commit(name, version, integrity, entry, title)
///   webfront.pkg.list() -> one "name\tversion\tentry URL\ttitle" line per installed tool
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

    ToolManager(App& app, Terminal<App>& terminal) : toolStore(std::make_shared<fs::ToolStore>(Location::get())) {
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
                registry->write(name, packageVersion, {.path = path, .offset = tools::detail::toOffset(offset), .bytes = bytes});
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
        app.template cppFunction<std::string>("webfront.pkg.list", [registry = toolStore] {
            std::string lines;
            for (const auto& [name, record] : registry->installed())
                lines += name + "\t" + record.version + "\t" + PackageFS::url(name, record.entry) + "\t" + record.title + "\n";
            return lines;
        });
        terminal.command("wf", "Manage web tools: wf list | wf remove <package>", [registry = toolStore](CommandArgs args, std::ostream& output) {
            tools::detail::runWf(*registry, args, output);
        });
    }

    [[nodiscard]] fs::ToolStore& store() {
        return *toolStore;
    }

private:
    std::shared_ptr<fs::ToolStore> toolStore;
};

template <typename App>
ToolManager(App&, Terminal<App>&) -> ToolManager<App>;

}  // namespace webfront
