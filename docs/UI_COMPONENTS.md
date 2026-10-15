# UI components

Status: design note, 2026-10-16. Stage 1 (the terminal component, #230) and stage 2 (the `wf` tool
manager, #234) are implemented.

WebFront's core is a server and a bridge. A *UI component* is a reusable piece of browser UI with a
C++ façade, packaged so that an application gets it from a header include. The first component is an
interactive terminal: it is useful on its own (a command-line interface to a C++ application) and is
the host for the tool manager.

## Anatomy of a component

A component has four parts.

| Part | Rule | Terminal |
| --- | --- | --- |
| Assets | An embedded `fs::Provider` serving under the reserved prefix `webfront/components/<name>/`, so it never collides with a development document root or the application's own pages. Readable sources live in `src/components/<name>/`, third-party code in `third_party/`, and generated headers are checked in and kept fresh by CTest, like `WebFront.js`. | `fs::TerminalFS` (`include/system/TerminalFS.hpp`) |
| Browser module | A native ES module exporting `mount(element, options)`. It depends only on the global `webFront` bridge and its own assets (relative imports), and waits for `webFront.ready`. | `src/components/terminal/terminal.mjs` |
| C++ façade | A header under `include/components/` that registers its bridge functions under `webfront.<name>.<function>`. `BasicWF::cppFunction` ignores a second registration of the same name, so the prefix is what keeps components and applications apart. | `webfront::Terminal` (`include/components/Terminal.hpp`) |
| Wire format | Only the existing bridge types: scalars, strings (at most 65535 bytes), tuples and numeric typed arrays. Lists of words cross as `"\n"`-separated strings. A component never changes `Messages.hpp` or `WebFront.js`. | `run(line) -> [output, module, args]`, `complete(line) -> candidates` |

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
- **Commands continuing in the browser:** any handler shape may return a
  `webfront::BrowserCommand{module, args}` (or a `std::optional` of one) for work the browser does
  better, such as HTTPS downloads. C++ still parses the line, so help, completion and argument
  checks stay in one place. After showing the command's output, the page imports `module` and awaits
  its exported `run(args, io)`, where `io.write(text)` prints (with `"\n"` line endings),
  `io.bridge` is the `webFront` bridge and `io.signal` is an `AbortSignal` that Ctrl+C aborts. The
  prompt stays locked until `run` settles, and a rejection prints `Error: <message>`. The module
  must be an absolute path on the page's origin (`/…`, not `//…`) without backslashes or control
  characters, and the arguments must not contain NUL, otherwise the command throws
  `std::invalid_argument`. Over the bridge, `run(line)` returns the output, the module (empty for
  none) and the arguments, each followed by a NUL character.
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

- Output of a C++ command appears when it finishes. There is no streaming and no cancellation. A long
  command keeps the prompt locked, and Ctrl+C cannot interrupt it. A command continuing in the
  browser can write progressively and stop on Ctrl+C, if its module honors `io.signal`.
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
| `wf open <pkg>` | Open `webfront/tools/<pkg>/<entry>` in a new tab or window |

### Installation flow

The download runs in the browser, which already speaks HTTPS. This keeps TLS out of the
header-only C++ library.

1. The browser fetches the packument from `registry.npmjs.org/<pkg>` (CORS-enabled) and resolves
   the version: an exact version or a dist-tag, `latest` by default. Version ranges are not
   supported. The version's manifest must have `"webfront": {"kind": "tool"}`.
2. It downloads the tarball, then checks `dist.integrity` with `crypto.subtle`, using the strongest
   of its SHA-512, SHA-384 or SHA-256 hashes; a package with only a SHA-1 `shasum` is refused.
   `crypto.subtle` needs a secure context, which `localhost` is.
3. It gunzips with `DecompressionStream("gzip")`, refusing more than 256 MiB, and reads the archive
   with a small tar reader (ustar with its `prefix` field, pax `path` records and GNU long names;
   links and directories are skipped). The first path component, `package/` in npm tarballs, is
   removed. The tarball's `package.json` must name the requested package and version and be a
   tool; `webfront.entry` defaults to `index.html` and must be in the tarball, and `webfront.title`
   defaults to the description, then the name.
4. It calls `webfront.pkg.begin(pkg, version)`, then for each file
   `webfront.pkg.writeFile(pkg, version, path, offset, Uint8Array)` in chunks of 256 KiB. Progress
   is shown in the browser, so no C++ push is needed. Browsers send such large messages in several
   WebSocket frames, which the server reassembles.
5. C++ rejects absolute paths, empty, `.` and `..` components, backslashes, and the characters Windows forbids in file names, and a chunk
   whose offset does not follow the bytes already written. Files are staged under
   `<project>/.webfront/tools/.staging/`.
6. `webfront.pkg.commit(pkg, version, integrity, entry, title)` checks that the entry page was
   written, moves the staged files to `<project>/.webfront/tools/<pkg>@<version>/`, replacing any
   installed version, and records the tool in `<project>/webfront-tools.lock.json`. A failed or
   abandoned installation leaves the installed version untouched.
7. `fs::PackageFS` serves `webfront/tools/<pkg>/…` from the versions recorded in the lock file,
   which it reads on each request. The prefix stays inside the reserved `webfront/` namespace, so a
   tool never collides with the application's pages.

### Implementation

```cpp
#include <components/ToolManager.hpp>

webfront::ToolsApp app;  // DefaultFSWith<TerminalFS, PackageFS>
webfront::Terminal terminal{app};
webfront::ToolManager tools{app, terminal};  // adds 'wf' and the webfront.pkg.* functions
```

`src/Terminal.cpp` (target `WebFrontTerminal`) includes it.

- `wf search`, `wf install` and `wf open` are browser commands (see the terminal component). C++
  checks their arguments and the package name, then the page runs
  `/webfront/components/tools/wf.mjs` (readable source `src/components/tools/wf.mjs`, embedded and
  served by `fs::PackageFS`). `wf open` receives the URL of the installed entry page from C++ and
  calls `window.open`; if the browser blocks the window, it prints the URL instead. Ctrl+C stops a
  download or an installation between two chunks.
- `ToolManagerOptions::registry` selects the registry, `https://registry.npmjs.org` by default.
  The browser test serves a local registry built by `webtest/MakeToolRegistry.cmake`.
- `fs::ToolStore` (`include/system/ToolStore.hpp`, with the small JSON reader of `include/utils/Json.hpp`) validates npm names (`@scope/` allowed),
  versions and paths, stages and commits installations, and reads and writes the lock file through
  a temporary file renamed over it. The lock file is read strictly, and a record whose name,
  version or entry could leave the tools directory is rejected. Names, versions and paths may not
  contain `..` anywhere, because the HTTP server rejects such URLs, and titles and integrities may not
  contain control characters, which would break the `webfront.pkg.list()` format, nor exceed 1024
  bytes, so that every line fits in a list page. Stores of the same project share one lock and one
  set of pending installations within the process; versions differing only by case, which name the
  same directory on a case-insensitive volume, replace each other without losing files. Once the lock file
  is written, a directory that cannot be deleted is left behind instead of failing the operation.
- `fs::BasicPackageFS<Location>` (`include/system/PackageFS.hpp`) takes the project directory from
  `Location::get()`; `fs::PackageFS` uses the working directory at startup (`fs::CurrentDirectory`).
  `ToolManager<App, Location>` must use the same `Location`.
- `webfront.pkg.list(first)` returns the `name\tversion\tentry URL\ttitle` lines of the tools from
  index `first`, as many as fit below the bridge's string limit, for `wf open`; the page calls it again
  from the next index until it gets an empty string. The URL is absolute and percent-encoded, so every accepted file name reaches `PackageFS`.
- The registered functions share ownership of the store, so a `ToolManager` may be destroyed before
  the application.

The lock file is meant to be committed with the project, the `.webfront/` directory is not:

```json
{
  "lockfileVersion": 1,
  "tools": {
    "@acme/webfront-plot": {
      "version": "1.2.0",
      "integrity": "sha512-…",
      "entry": "dist/index.html",
      "title": "Live plot"
    }
  }
}
```

### Security

An installed tool runs in the application's origin and can call every registered C++ function. The
trust model is therefore that of `npm install`: only install tools you trust, and only during
development. A later step can serve tools from a separate origin (another port) with an allow-list of
callable functions. The tool manager is a development feature, like a document root, not a
production plugin system.

### Tests

- Catch2: `ToolStoreTests.cpp` (validation, staging, lock file, in temporary directories),
  `ToolManagerTests.cpp` (bridge functions, `wf` commands and the arguments of their browser part,
  `PackageFS`), and `WebSocketTests.cpp` for fragmented messages.
- Jasmine (`webtest/js/wf-specs.mjs`), against the local registry: search; installation of a tool
  with a file larger than a chunk, an empty file and long paths, then of a scoped tool at a given
  version; reinstallation; `wf open`; a missing package or version, a wrong hash, a package that is
  not a tool, a tarball for another package, a path escaping the tool (rejected by C++, the other
  tools staying installed) and Ctrl+C during a download. No test needs the public network.

### Limitations

- Version ranges such as `^1.2` are not resolved; dependencies of a tool are not installed, so a
  tool must ship its built files.
- `wf search` shows the first 20 results.
- An interrupted or failed installation leaves its staged files until the next installation of the
  same version.
