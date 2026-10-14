# UI components

Status: design note, 2026-10-14. Stage 1 (the terminal component, #230) is implemented. Stage 2
(the `wf` tool manager, #234) is designed here but not implemented.

WebFront's core is a server and a bridge. A *UI component* is a reusable piece of browser UI with a
C++ façade, packaged so that an application gets it from a header include. The first component is an
interactive terminal: it is useful on its own (a command-line interface to a C++ application) and is
the host for the planned tool manager.

## Anatomy of a component

A component has four parts.

| Part | Rule | Terminal |
| --- | --- | --- |
| Assets | An embedded `fs::Provider` serving under the reserved prefix `webfront/components/<name>/`, so it never collides with a development document root or the application's own pages. Readable sources live in `src/components/<name>/`, third-party code in `third_party/`, and generated headers are checked in and kept fresh by CTest, like `WebFront.js`. | `fs::TerminalFS` (`include/system/TerminalFS.hpp`) |
| Browser module | A native ES module exporting `mount(element, options)`. It depends only on the global `webFront` bridge and its own assets (relative imports), and waits for `webFront.ready`. | `src/components/terminal/terminal.mjs` |
| C++ façade | A header under `include/components/` that registers its bridge functions under `webfront.<name>.<function>`. `BasicWF::cppFunction` ignores a second registration of the same name, so the prefix is what keeps components and applications apart. | `webfront::Terminal` (`include/components/Terminal.hpp`) |
| Wire format | Only the existing bridge types: scalars, strings (at most 65535 bytes), tuples and numeric typed arrays. Lists of words cross as `"\n"`-separated strings. A component never changes `Messages.hpp` or `WebFront.js`. | `run(line) -> output`, `complete(line) -> candidates` |

Assets are added to an application through its filesystem type. `fs::DefaultFSWith<Extra...>` is
`fs::DefaultFS` with extra providers placed after the protected `WebFront.js` and before the
document root, so a development root can neither replace the bridge nor shadow a component file:

```cpp
using TerminalApp = webfront::BasicWF<webfront::NetProvider, webfront::fs::DefaultFSWith<webfront::fs::TerminalFS>>;
```

Components are opt-in. `webfront::WebFront` does not include their assets, so its binary does not
grow, and `#include <WebFront.hpp>` alone does not pull them in.

## The terminal component

```cpp
#include <WebFront.hpp>
#include <components/Terminal.hpp>

int main() {
    webfront::TerminalApp app;
    webfront::Terminal terminal{app, {.prompt = "demo> "}};
    terminal.command("greet", "Greet someone", [](webfront::CommandArgs args) {
        return "Hello, " + (args.empty() ? std::string{"world"} : args[0]) + "!";
    });
    app.show(terminal.page);
}
```

`src/Terminal.cpp` (target `WebFrontTerminal`) is the complete example.

- **Rendering:** [xterm.js](https://xtermjs.org/) 6.0.0 and its fit addon (MIT; the terminal used by
  VS Code, Tabby and Hyper), vendored verbatim in `third_party/xterm/` and embedded (about 350 KB).
  ghostty-web, Ghostty's emulator compiled to WebAssembly with an xterm-compatible API, was
  considered and set aside for now: it is under heavy development and supports Chrome only.
- **Line editing happens in the browser.** The editor in `terminal.mjs` handles insertion, left and
  right, Home and End (also Ctrl+A and Ctrl+E), Backspace and Delete, Ctrl+U, history (up and down),
  Tab completion of command names, Ctrl+C (cancel the line) and Ctrl+L (clear the screen). C++
  only receives validated lines, one at a time. The prompt stays locked until the Promise for the
  line settles.
- **Commands** are registered by name with a help text and a handler of one of three shapes:
  `(CommandArgs, std::ostream&)`, `(CommandArgs) -> R` or `() -> R`, where `R` is `void` or anything
  that can be written to a `std::ostream`. `CommandArgs` is a `std::span<const std::string>`
  without the command name. The line is split shell-style: single quotes are literal, double quotes
  honor backslash escapes, and an unquoted backslash escapes the next character. `help` and `clear`
  are built in. Duplicate, empty or whitespace-containing names throw `std::invalid_argument`.
- **Output and errors:** the command output is the Promise result, with `"\n"` converted to
  `"\r\n"` and ANSI escape sequences passed through. An unknown command or a malformed line prints
  a red message. An exception thrown by a command rejects the Promise, and the page prints
  `Error: <what()>`. Output and error messages (which may quote the line) are capped at 60000 bytes, cut on a UTF-8 character boundary, with a
  notice, because bridge strings carry a 16-bit length.
- **Lifetime and threads:** the registered bridge functions share ownership of the command
  registry, so the `Terminal` object may go out of scope before the application. The registry is
  guarded by a mutex: commands may be added while the server thread runs others, and commands run
  on the server thread.

### Limitations of the first version

- Output appears when a command finishes. There is no streaming and no cancellation. A long command
  keeps the prompt locked, and Ctrl+C cannot interrupt it.
- Every connected browser shares the same commands. The terminal does not know which browser sent
  a line.
- The line editor redraws within one terminal row, so a line wider than the terminal displays
  incorrectly. Wide (CJK) characters count as one column.
- There is no `exit` command. With the system browser, `show()` waits for Enter on standard input,
  and stopping the server from inside a command would drop the reply to that command.
- A multi-line paste runs its first line only.

## Core gaps found by the terminal

These belong to the core, not to the component. Stage 1 did not change them; the first two are
now resolved:

1. `onUIStarted` keeps a single handler, which a component would replace. Resolved by #233:
   components call `subscribeUIStarted(handler)` and keep the returned `UISubscription`.
2. A `cppFunction` callback did not know which `UI` called it. Resolved by #233: a callback whose
   first parameter is the application's `UI` receives the calling browser, so a component can answer
   later or stream to it.
3. Outstanding calls cannot be cancelled (already listed in `PROJECT_STATUS.md`).
4. Bridge strings are limited to 65535 bytes. Until #231 the C++ encoder wrapped longer lengths and
   corrupted the message; it now rejects them (the JavaScript encoder always did). The terminal caps
   its output and error messages below that limit so they are shown rather than rejected.

## Stage 2 design: the `wf` tool manager (#234)

Goal: install web-based development tools (log viewers, memory or protocol inspectors, plotters…)
into a WebFront application while debugging it, the way `npm install` adds a package.

### Packages

A tool is an ordinary npm package whose `package.json` has a `webfront` field and the
`webfront-tool` keyword:

```json
{
  "name": "@acme/webfront-plot",
  "version": "1.2.0",
  "keywords": ["webfront-tool"],
  "webfront": { "kind": "tool", "title": "Live plot", "entry": "dist/index.html" }
}
```

Reusing npm provides hosting, versioning, `dist.integrity` hashes and search without running a
registry.

### Commands

`wf` is a terminal command, plus browser-side code for the network part:

| Command | Effect |
| --- | --- |
| `wf search <term>` | npm search API restricted to `keywords:webfront-tool` |
| `wf install <pkg>[@version]` | Download, verify, unpack, store, record in the lock file |
| `wf list` | Installed tools with version and title |
| `wf remove <pkg>` | Delete the tool's files and lock entry |
| `wf open <pkg>` | Open `tools/<pkg>/<entry>` in a new tab or window |

### Installation flow

The download runs in the browser, which already speaks HTTPS. This keeps TLS out of the
header-only C++ library.

1. The browser fetches the packument from `registry.npmjs.org/<pkg>` (CORS-enabled) and resolves
   the version.
2. It downloads the tarball, then checks `dist.integrity` (SHA-512) with `crypto.subtle`.
3. It gunzips with `DecompressionStream("gzip")` and reads the archive with a small ustar reader
   (about 60 lines).
4. For each file it calls `webfront.pkg.writeFile(pkg, version, path, Uint8Array)`. Files are sent
   in chunks to stay under the bridge's frame limits; typed arrays already cross the bridge.
   Progress is shown in the browser, so no C++ push is needed.
5. C++ rejects absolute paths and `..` components, writes under
   `<project>/.webfront/tools/<pkg>@<version>/`, and updates `webfront-tools.lock.json` (name,
   version, integrity, entry) after the last file. The project directory is configurable.
6. A new `fs::PackageFS` serves `tools/<pkg>/…` from the installed versions recorded in the lock
   file.

### Security

An installed tool runs in the application's origin and can call every registered C++ function. The
trust model is therefore that of `npm install`: only install tools you trust, and only during
development. A later step can serve tools from a separate origin (another port) with an allow-list of
callable functions. The tool manager is a development feature, like a document root, not a
production plugin system.

### Prerequisites

- Core gaps 1 and 2 above, for progress and output streaming initiated by C++ (done in #233).
- A filesystem provider writing outside the build tree, with tests using temporary directories.
- Browser tests that use a local fake registry: tests must not need the public network.
