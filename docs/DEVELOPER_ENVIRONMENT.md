# Browser-powered development

# The WebFront developer environment

The developer environment is the tooling of WebFront: a terminal, a package installer, a TypeScript/TSX compiler, a type
checker, a test runner and an asset generator, all running in the browser of your application. The C++ executable holds the
bridge and a small bootstrap loader; at the first launch the loader downloads the environment, verifies its SHA-512 and
caches it, so later launches work offline. Nothing else has to be installed: a C++ compiler, CMake and the selected native
webview are enough.

A project is a directory with a manifest, `webfront.project.json`, and a lock, `webfront-packages.lock.json`. The `wf`
commands of the terminal operate on it:

| Command | Purpose |
| --- | --- |
| `wf init`, `wf create <starter>` | write the manifest; copy the Life starter into a new directory |
| `wf install`, `wf restore` | resolve, download and lock packages; rebuild or verify the cache from the lock |
| `wf dev`, `wf build`, `wf preview` | rebuild and reload the preview on every change; build once; preview once |
| `wf check` | type-check with the locked TypeScript compiler |
| `wf test` | run Jasmine specs in the application's window |
| `wf run`, `wf exec` | run a manifest script or a package's executable entry |
| `wf embed` | generate the C++ provider of the application's resources |

**Limits, stated directly.** The downloaded environment and the scripts you authorize run with the application's bridge
authority and may read and write the selected project: there is **no sandbox**, and the manifest's permissions frame only the
`context` API given to scripts. Only packages that run in a browser are installed; their installation scripts and native
add-ons are not executed, and the APIs of server-side JavaScript runtimes are not available. No shell is exposed. Do not
ship developer mode as the distribution configuration: `wf embed` produces the application that you distribute.

**TypeScript** is supported throughout. TS and TSX are compiled by esbuild-wasm in the browser, which removes the types;
`wf check` verifies them with the TypeScript 5.x or 6.x compiler that your lock pins, using your `tsconfig.json`, the
`.d.ts` files and the type packages (`@types/*`, `types` and `typings` fields) of the locked graph. No external compiler is
installed or required (see [Type checking](#type-checking-wf-check)).

The generated `DeveloperReleaseData.hpp` selects the default environment and its integrity. Published
versions stay immutable. Environment **0.6.3** adds automation API v1 and the shared human/agent execution
lease; version 0.6.2 predates that API. Publication and pin changes follow
[PUBLISHING](../js/developer/tools/PUBLISHING.md), with deployment verified before merging a new pin.

## Minimal C++ application

Enable the platform's native frontend in CMake: `WEBFRONT_EMBED_WKWEBVIEW=ON` on macOS,
`WEBFRONT_EMBED_WEBVIEW2=ON` on Windows, or `WEBFRONT_EMBED_WEBKITGTK=ON` on Linux.
The default `webfront::WebFront` still opens the system browser.

```cpp
#include <webfront/WebFront.hpp>
#include <bitset>

std::bitset<1024> nextLife(const std::bitset<1024>& cells); // Existing C++ simulation.

int main() {
    webfront::NativeApp app{"."}; // Explicit development project directory.
    app.cppFunction("life.step", &nextLife);
    app.bootstrap("developer"); // Uses the version and SHA-512 pinned in WebFront.
    app.show();
}
```

The [versioned distributions](https://ambroise-leclerc.github.io/WebFront/) are published on GitHub Pages.
Each published version is immutable: 0.3.0 introduced the packed-bitset Life starter (bridge support from #289), 0.3.1 made
`wf install` keep the versions of the installed lock, 0.4.1 changed the include of the generated provider, 0.5.0 added the
manifest, `wf restore`, `wf run`, `wf exec`, `wf check` and the complete terminal, and 0.6.0 `wf build`, `wf dev` and `wf test`.

The default configuration is generated in `include/webfront/system/DeveloperReleaseData.hpp`; it contains
an exact versioned environment URL and the trusted digest. The 0.6.3 release pin selects
`https://ambroise-leclerc.github.io/WebFront/developer/0.6.3/environment.tgz`; the
[publication gate](validation/developer-0.6.3-release.md) requires deployed verification before that pin is merged.
Published 0.6.2 introduced installation of the archives of `@types` packages and the manifest directory;
0.6.1, 0.6.0, 0.5.0 and 0.4.1 stay immutable, while 0.4.0 and earlier target the former include layout.
The configuration pins one version and its SHA-512 rather than resolving a mutable `latest` channel. An explicit
`DeveloperBootstrapOptions{.environment = url, .integrity = trustedHash}` still selects a different
HTTPS/CORS repository or a same-origin test fixture. Users install no JavaScript tooling.

The current environment archive contains the pinned Life starter descriptor. `wf create life` downloads and
verifies that separate archive, creates a new `ui` directory and prints the install/preview/embed
commands. Existing directories are never replaced. The trusted environment hash protects both
its own code and the starter descriptor; `provision.json` on the server is not a new trust anchor.
See [publication instructions](../js/developer/tools/PUBLISHING.md) for deployment and release checks.

The bootstrap verifies SHA-512 before unpacking or executing the archive, validates its manifest
and paths, then publishes it under the application's local origin. The cached **archive** is
verified again before reuse, allowing subsequent launches without network access. Failure leaves
a readable status and Retry button. A different pinned hash requires a matching archive; the old
environment is not silently substituted.

## The Game of Life walkthrough

`examples/life/Life.hpp` is a small C++23 B3/S23 simulation on a `std::bitset<1024>`.
Each generation reads the old grid and creates a new one; the 32 × 32 world wraps on both axes.
The core needs no React, server, thread, timer or graphics dependency. `nextLife` is registered directly
with its `const LifeGrid&` argument and bitset result. React stores and exchanges a 128-byte `Uint8Array`,
with cell `i` in bit `i % 8` of byte `i / 8`. The bridge validates the exact unsigned-byte type and length.
`examples/life/LifeConsole.cpp` runs it without WebFront; build and run `WebFrontLifeConsole` to start
with a plain console application.

`examples/life/LifeDeveloper.cpp` adds the native window and downloaded developer bootstrap. Configure
`build-native` with the native frontend option above and start with a fresh project directory:

```sh
mkdir -p life-project
./build-native/examples/WebFrontLifeDeveloper "$PWD/life-project"
```

Optional extra arguments `<environment-url> <sha512-integrity>` override the pinned default (for instance with the local
archive of [the maintainers' section](#environment-distribution-and-offline-tests-maintainers)). In the downloaded terminal:

```text
wf create life
wf init
wf install react@19.3.1 react-dom@19.3.1
wf dev ui/main.tsx
wf install jasmine-core@7.0.2
wf test
wf embed ui/main.tsx --output generated/LifeUI.hpp
```

`wf test` needs `test.specs` declared in the manifest and `jasmine-core` installed; type checking is a separate step: `wf install typescript@5.9.3 @types/react@19.3.0 @types/react-dom@19.3.0` then `wf check` need a `tsconfig.json` (see the README for a complete one). The terminal does not read `#` comments, so type the commands only.

The starter contains editable TSX/CSS and a recipe of exact versions; `wf init` does not copy it, so list the dependencies with `wf install` and declare `test.specs`, `entries` and `providers` in the manifest yourself.
`wf create life another-directory` selects a different new destination; existing directories
are never replaced. React DOM pulls scheduler transitively and checks its React peer.
Edit the local files in your normal editor and use `wf dev` for automatic rebuilds, or run `wf preview` again. Ctrl+C interrupts
commands, including downloads and builds.

The interface draws an isometric field with raised living cells, soft lighting and translucent
panels. Play/pause, single step, tempo, glider/blinker/galaxy seeds, clear, cell painting and a plan
view make the simulation interactive. Generation and population counters expose its state.
Every step awaits `webFront.cppFunction('life.step')(cells)`; no Life rules run in JavaScript.
Only one generation request can be outstanding. Replacing a pattern or clearing the field stops
animation and discards stale replies. Errors stop playback and appear in the interface.
Canvas rendering has an accessible description; patterns and simulation controls work with the
keyboard. Individual canvas cells are pointer-editable, without keyboard cell navigation.
No fonts, images, 3D library or additional UI framework are downloaded.

![The native Game of Life demo at generation 26](images/life-preview.jpg)

`wf embed` builds JS/CSS/resources, creates an HTML entry page, preserves dependency notices and
writes a header-only `webfront::fs::LifeUI` provider using deterministic gzip. The filename stem
determines the C++ provider name. The header is replaced only after all chunks arrive successfully.
The distribution form has no project root or developer callbacks:

```cpp
#include <webfront/WebFront.hpp>
#include "Life.hpp"
#include "generated/LifeUI.hpp"

int main() {
    webfront::NativeAppWithAssets<webfront::fs::LifeUI> app;
    app.cppFunction("life.step", &webfront::demo::nextLife);
    app.show();
}
```

Reconfigure the native build with
`-DWEBFRONT_LIFE_ASSETS=/absolute/path/to/life-project/generated/LifeUI.hpp`, build target
`WebFrontLifeEmbedded` and run it. Recompilation is explicit. The resulting binary needs neither
project sources, downloaded cache nor network.

The current distribution provides the Life starter; the retired PI starter is no longer shipped.
The former embedded React/Babel providers have been removed; use the generated application provider.

## Project manifest (`webfront.project.json`)

Available from developer distribution 0.5.0.
A project has one manifest, which every `wf` command reads:

```json
{
  "format": 1,
  "name": "life",
  "version": "0.1.0",
  "dependencies": {"react": "^19.3.1", "react-dom": "^19.3.1"},
  "registry": "https://registry.npmjs.org/",
  "scripts": {},
  "entries": {"main": "ui/main.tsx"},
  "providers": {"LifeUI": {"entry": "main", "output": "generated/LifeUI.hpp"}},
  "test": {"specs": []},
  "permissions": {}
}
```

Only `format`, `name` and `version` are required. Validation is strict: an unknown key, a format
newer than the environment reads, a path leaving the project, a non-HTTPS registry or origin and an
unknown permission are errors printed in the terminal, never silently ignored. `scripts` and `permissions` are
used by `wf run` and `wf exec` (below); `test.specs` lists the spec files of `wf test`. Permissions only frame
the operations of the `context` API of scripts; they are not a sandbox.

- `wf init [name]` writes `webfront.project.json` at the project root (or in the application's manifest directory, below),
  the manifest every `wf` command reads; it never replaces an existing one. When a lock already exists (a project installed before
  0.5.0), the manifest takes its roots as dependencies (`^version`, or the range they were installed with).
- The application may keep the manifest and its lock in a directory of the project instead of its root, from environment
  0.6.2: `DeveloperBootstrapOptions::manifestDirectory = "js/developer"` (or `--manifest-dir js/developer` for the test driver).
  The directory is relative to the project, `/`-separated, without `..` and outside `.webfront/` (which `wf` manages); a refused value stops the bootstrap. The choice is the
  application's, never the manifest's, so that a manifest cannot widen the project. Only the two files move: every path
  inside the manifest (entries, scripts, specs, permissions) and the cache `.webfront/` stay relative to the project root,
  and the manifest and the lock remain files that scripts never write. An earlier environment ignores the option and looks
  in the project root.
- `wf install <package>[@range]` resolves, downloads and verifies into the cache, then publishes the
  manifest (the requested range, or `^version` when none was given) and the lock in a single transaction.
- `wf install` without argument installs exactly the locked graph when the lock agrees
  with the manifest, resolves the manifest and creates the lock when there is none, and fails when they
  disagree. `wf install --update` resolves the manifest again.
- `wf preview [entry]` and `wf embed <provider>` use `entries` and `providers`; an explicit path still works.
  `wf embed <provider>` generates the C++ class `webfront::fs::<provider>`; with `--output <Provider.hpp>`,
  the file name must be a C++ class name.
- A requested range is checked like the manifest that records it, before anything is resolved: a range or
  tag the manifest cannot hold is refused instead of written. The manifest is rewritten with the keys it had,
  plus the new dependencies.

The manifest holds the intent (roots and ranges), the lock the exact resolution. The lock records a
digest of the manifest's `dependencies` (`manifestDigest`). The project is **consistent** when they
match, **unlocked** without a lock (no installed graph) and **desynchronized** when the dependencies
changed since the lock was written; every command reports that state and none resolves implicitly.
A lock written before 0.5.0 has no digest: it is used as consistent when its roots satisfy the
dependencies (after `wf init`, they do), and `wf install` records the digest without resolving anything.

Both files are published by one all-or-nothing transaction (`developer::Store::commitGroup`): the
staged files are moved under `.webfront/developer/journal/`, a journal naming them is written and
synchronized to disk (the commit point), then each file replaces its target. Opening the project
completes a journal that reached its commit point and abandons the others, so a crash never leaves
a manifest and a lock that belong to different installations. A failure before the commit point returns
the staged files to their transactions. A committed publication whose replacement fails (a target turned
into a directory, a file locked by another program) is reported as committed but not applied yet; its
journal is kept, the project still opens, and every later command first completes it, failing with the
cause until it can. The publication carries a precondition,
the fingerprint of both files as read when the command began: if another browser or command changed
them meanwhile, it is refused with "The project changed since it was read; run the command again" and
nothing is lost. Recovery and group publications also hold an advisory lock on `.webfront/developer/journal/.lock`, so
several processes sharing a project (or several stores of one process) are serialized rather than abandoning one
another's publication; other project files stay unprotected against concurrent local modification. `wf restore` fills the cache from the lock without ever writing them.

## Restoring the cache and choosing the registry (`wf restore`)

The cache under `.webfront/developer/packages/` is not committed. On a new machine, after a clone, or
when it is damaged, `wf restore` rebuilds it from the lock alone:

- each locked package is downloaded at its exact locked version (no range is resolved again) and checked
  against the lock's SHA-512 integrity; packages already valid in the cache are not downloaded again, so the
  command is idempotent;
- every package is published on its own: Ctrl+C or an error (changed archive, package absent from the
  registry) keeps the packages already restored and never writes the manifest or the lock; run the command again;
- `wf restore --offline` contacts no registry at all: it checks the cache against the lock and fails with the
  list of missing or corrupted packages, without downloading anything.

Each cached package directory `<key>/` is followed by a record `<key>.json` naming the archive's integrity and the
SHA-256 of every file; the record is written last, so an interrupted copy is *missing*, and a file that differs
from its record, or exceeds the bound below, is *corrupted*. The 256 MiB bound applies to the whole unpacked graph,
packages already in the cache included, and is checked before each package is published, so resuming a restore
cannot exceed it. `wf install` with a consistent lock performs the same restoration.

The registry used by `wf install` and `wf restore` is, in order of precedence:

1. the application's `DeveloperBootstrapOptions::registry` (the distributor's trusted channel; a test driver
   uses it to impose its local registry),
2. the manifest's `registry`,
3. `https://registry.npmjs.org/`.

Both commands print the registry in use and where it comes from, for example
`Registry: /mirror/ (application option)`. A registry must use HTTPS or be a path on the application's own
origin, like the environment archive (a path starting with `//` or containing a backslash, which browsers read as
another host, is refused); tarballs must use HTTPS or the registry's origin. The lock's `registry`
field records where the graph was resolved and is not used to restore it, so a mirror can serve a lock resolved
from the public registry.

Offline provisioning, extending ADR-013's explicit local provisioning: serve a directory of packuments (`<name>`)
and tarballs from the application (`fs::NativeDebugFS` or an application provider) and pass its path as
`registry`, then run `wf restore`; or copy `.webfront/developer/packages/` from another machine and check it with
`wf restore --offline`. The environment archive is provisioned the same way (see below).

## Scripts (`wf run`, `wf exec`)

WebFront scripts produce a project's resources, in the browser:

```json
{
  "scripts": {"icons": "tools/icons.ts --size 64", "build": "wf embed LifeUI"},
  "permissions": {"files": ["assets"], "publish": ["generated"], "fetch": ["https://api.example.org"]}
}
```

- `wf run [--timeout <seconds>] <script> [args...]` runs a script of the manifest. A script whose first word is `wf`
  runs that wf command (up to four nested levels); otherwise its first word is the project path of a JS or TS module
  and the other words, then the command's arguments, are its `context.args`. No shell is involved.
- `wf exec [--timeout <seconds>] <package>[:<entry>] [args...]` runs an executable WebFront entry of an installed
  package: the field `webfront.bin` of its `package.json`, a path or an object naming paths. npm's `bin` is never run:
  a package with only a `bin` (a Node.js program) is refused, as is a browser package without `webfront.bin`.

The module is bundled by esbuild-wasm with its locked dependencies (JSX with the automatic runtime, as for previews)
and runs in a dedicated Worker; only the package files it imports are read from the cache. It exports
`default async function (context)`, where `context` offers:

| Member | Behavior |
| --- | --- |
| `args` | The arguments, as an array of strings. |
| `signal` | An `AbortSignal` aborted by Ctrl+C and by the time limit. |
| `console` | `log`/`info`/`debug` and `warn`/`error`, streamed to the terminal; the Worker's global `console` too. |
| `project.read(path)`, `readText(path)` | A project file as bytes or text, under `permissions.files` or `publish`. |
| `project.list(path = '')` | The names in a project directory (directories end with `/`). A readable directory is listed whole; the root and the directories leading to granted paths list only the entries leading to them. |
| `project.write(path, data)` | Publishes a string or `Uint8Array` with `developer::Store`, under `permissions.publish`. |
| `packages.resolve(name)` | `{name, version, integrity}` of a locked package, or `null`. |
| `fetch(input, init)` | `fetch` of a URL or a `Request` to an origin listed in `permissions.fetch`; a refusal rejects. Redirects are not followed, since their target could be any origin: the promise rejects and the script requests the final URL. |
| `exit(code)` | Ends the run with an exit code from 0 to 255; it throws, so the statements after it do not run, and nothing the script does afterwards reaches the page. |

A `context` operation outside the permissions rejects with an explicit error. The manifest, the lock and
`.webfront/` are never written by a script, whatever the permissions. **The permissions frame only the `context`
API**: the Worker keeps its own capabilities (global `fetch`, `importScripts`, WebSocket...), and nothing here is a
security boundary. Node.js modules (`fs`, `child_process`, `node:*`...) are refused while the script is loaded,
unless a locked package of that name is a dependency of the importer; `process` does not exist and no polyfill is
provided.

Ctrl+C first aborts `context.signal`; a script still running two seconds later (the grace period) is terminated with
`worker.terminate()`, which also stops a loop that never returns to the event loop. The time limit (`--timeout`,
600 seconds by default, from 1 to 3600) applies the same sequence; it also bounds the scripts that the script runs
through `wf` commands, whatever their own limit. The terminal stays usable. Each write is a transaction of its own:
at a stop, one still staging is aborted and leaves the file unchanged, and one whose commit was already sent
completes atomically; the terminal lists the publications applied and abandoned. When a script ends by itself, the
writes it did not await complete first; if one of them fails, the run ends with code 1 and names it. A script that
returns ends with code 0, a thrown error with 1, `exit(code)` with that code, Ctrl+C with 130 and the time limit with
124; a non-zero code is an error carrying `exitCode`.

## Type checking (`wf check`)

Previews and providers are built by esbuild-wasm, which removes the types without checking them. `wf check
[--timeout <seconds>]` runs the TypeScript compiler of the project's lock on the project, in the browser:

```sh
wf install typescript@6
wf check
```

- **Supported versions**: `typescript` 5.x and 6.x (`>=5.0.0 <7.0.0`), whose `lib/typescript.js` provides the
  compiler API; 5.9.3 and 6.0.3 are tested. An older version, or TypeScript 7 (a native program without that API), is
  refused with an explicit message. The version is the one the lock records, never a copy shipped by WebFront.
- **Options**: the project's `tsconfig.json` if it exists (comments, `extends` of project or package files,
  `include`/`exclude`/`files`, `compilerOptions`). Otherwise the TS/TSX entries of `webfront.project.json` are the root
  files, with `target` ES2022, `module` ESNext, `moduleResolution` Bundler, `jsx` react-jsx, `lib` ES2022/DOM/
  DOM.Iterable, `strict`, `isolatedModules` and `skipLibCheck`. `noEmit` is always added (and `emitDeclarationOnly`
  removed), the other options keep their meaning; nothing is written, not even build information. Project references
  are not followed.
- **Two phases**: the check runs in a dedicated Worker. It first loads, asynchronously, an in-memory snapshot: the
  `lib.*.d.ts` of the locked `typescript`, the declaration files and `package.json` of every locked package, then the
  `.ts`/`.tsx`/`.mts`/`.cts` files (with `.js` family files under `allowJs`, `.json` under `resolveJsonModule`) and
  `package.json` files of the directories that `tsconfig.json` includes, or of the entries' directories, recursively,
  and the `package.json` files above them up to the project root, which give their module format.
  `node_modules`, `.webfront` and `.git` are never listed. TypeScript then compiles synchronously over that snapshot:
  a file absent from it does not exist, including a project file imported from outside those directories.
- **Type resolution** follows TypeScript, not esbuild: `types`/`typings`, the `types` condition of `exports`,
  `@types/*` and the `moduleResolution` of `tsconfig.json`. Only the locked graph is visible, without flattening: the
  project sees its roots, and each package its own dependencies.
- **Diagnostics** print as `file(line,column): error TScode: message`, with project-relative paths, or
  `<package>@<version>/<path>` inside a package, then a summary. Any error ends the check with code 1.
- **Bounds**: the snapshot holds at most 10,000 files and 64 MiB, and listing stops at 50,000 directory entries. A
  snapshot that reaches its bounds still compiles but reports the files it left out and fails; a listing past its
  bound fails at once. TypeScript itself (about 9 MiB) is read from the cache and is not counted. The Worker's memory
  is the browser's; it is not limited further.
- **Time**: `--timeout` (600 seconds by default, from 1 to 3600) and Ctrl+C stop the check like `wf run`: a check
  still loading its snapshot stops at its next read, and one compiling, which never returns to its event loop, is
  terminated with `worker.terminate()` after the two-second grace period. The exit code is 130 (Ctrl+C) or 124 (time
  limit), and the terminal stays usable.

## The terminal and the application's commands

The terminal of the environment (developer distribution 0.5.0, #308) is the only WebFront terminal; it is served
from the environment archive with xterm.js, never from a C++ header. It runs `wf` commands, the application's C++
commands and its own: `help`, `clear` (screen and scrollback), `history` (`history -c` clears it) and
`exit [code]`, which ends the session and closes the window; when the browser keeps a window that a script did not
open, Enter starts a new session. No system shell is exposed.

- **Editing:** left/right, Home/End (Ctrl+A/Ctrl+E), Backspace/Delete, Ctrl+U/Ctrl+K, Ctrl+L, up/down through the
  history. A line wider than the terminal wraps over several rows and is redrawn in place; a line taller than the
  terminal is drawn a screen of rows at a time around the cursor, since rows that left the screen cannot be rewritten. The editor moves over
  grapheme clusters: CJK characters and emoji (ZWJ sequences, flags, skin tones, presentation selector) take two
  cells, combining marks none. The same widths are registered in xterm.js, so both always agree.
- **History:** kept per project in `.webfront/developer/history.json` (500 lines), restored by the next terminal. Each
  line is added to the file as it is, with a conditional publication tried again on conflict, so terminals of several
  pages on the same project keep each other's lines. A history file that cannot be read is never replaced.
- **Typing ahead and paste:** keys typed or pasted while a command runs are applied after it, line by line; a last
  line without a line break waits on the prompt. Ctrl+C drops the keys still waiting.
- **Completion (Tab):** command names, `wf` subcommands, the manifest's scripts after `wf run`, its entries and
  entries/providers after `wf build`, `wf dev`, `wf preview` and `wf embed`, and project paths, also when the word is escaped (`my\ dir/`) or
  quoted (`"my dir/`); the completion is written the same way and closes the quote. Several candidates are completed
  to their common prefix, or listed.
- **Exit codes:** a failed command prints `Error: <message>`, with `(exit code N)` when N is not 1. Ctrl+C gives 130.
- **Output streams** while a command runs, for `wf` and C++ commands alike, without a size limit: C++ output is
  sent in pieces below the 65,535-byte limit of bridge strings.

An application adds its own commands with `webfront::Commands` (`<webfront/Commands.hpp>`). A command is a coroutine:

```cpp
#include <webfront/Commands.hpp>

webfront::NativeApp app{projectDirectory};
webfront::Commands commands{app};
commands.add("count", "Count to N, once a second", [](webfront::Commands<webfront::NativeApp>::Context context) -> webfront::Task {
    using namespace std::chrono_literals;
    for (int i = 1; i <= std::stoi(context.args().at(0)); ++i) {
        co_await context.print(std::to_string(i) + "\n");
        co_await context.ui().resumeAfter(1s);
    }
});
app.bootstrap("developer");
app.show();
```

The terminal discovers the commands through the bridge (`webfront.commands.list`) and runs each one in the session
of its browser with `webfront.commands.start`. In C++, the command is spawned like `ui.spawn` on the server thread:

- `context.write(text)` streams output with fire-and-forget calls; `co_await context.print(text)` sends it piece by
  piece and, after each piece, suspends the command while more than 32 frames wait for a browser that reads slower
  (`ui.writesAtMost`, woken by the write that drains the queue); each wait is a point where Ctrl+C stops it.
  `context.args()` holds the words after the command name, split like a shell line; `context.exit(code)` sets the
  exit code. A command that returns once asked to stop reports 130, whatever it returned.
- Ctrl+C sends `webfront.commands.cancel`: the command's stop token is requested, which the senders it awaits
  observe (`ui.resumeAfter`, `ui.jsFunctionSender`, ...); loops check `context.stopRequested()`. The terminal does
  not wait for the command to end: the prompt returns at once and the next command runs, while the first one
  unwinds. Its late output and exit report are ignored.
- The browser's disconnection stops its commands, and the application's shutdown drains them with its other tasks.
  In the terminal, a command whose connection closes ends at once with an error (exit code 1): its exit report can
  no longer arrive.
- An exception thrown by a command is printed as an error, with exit code 1.
- Each connected browser has its own session: a command in one browser neither delays nor is stopped by another.
- A command must not block the server thread, which serves every browser: a handler that does not return
  `webfront::Task` does not compile. Long computations belong on another thread.

A second `webfront::Commands` on the same application throws `std::logic_error` and registers nothing. A copy of a
`Context` may outlive its command (a detached task): its stop token stays valid and its writes are dropped once the
browser is gone. The names `wf`, `clear`, `exit`,
`help` and `history` are reserved. `context.write` has no flow control: a command
writing large output with it, faster than the browser reads, grows the connection's write queue; use
`context.print` for such output.

## Build and continuous development

`wf build [entry.tsx|name]` compiles without opening or navigating the preview and publishes the whole
result to `.webfront/developer/build/`. With no argument it builds every manifest entry: a single entry
uses that directory directly; multiple entries each get a named subdirectory with their own `index.html`,
`app.js`, optional CSS and assets. Directory names percent-escape punctuation and Windows device names. An explicit path or manifest entry name selects one entry at the root.
Publication replaces the directory only after every entry succeeds. Build output is available as local
files; the developer HTTP provider serves only the environment and preview directories.

`wf dev [entry.tsx|name]` selects `entries.main` or the sole entry when omitted, builds, opens the preview
and keeps running until Ctrl+C. It reuses one compiler Worker and esbuild context across rebuilds.
Changing the selected entry or locked graph recreates the context. It uses the installed lock, without
resolving packages or downloading dependencies; `wf install`/`wf restore` remain explicit actions.

Detection uses portable, sequential polling rather than a platform-specific C++ filesystem watcher.
After each build it waits 750 ms, then compares SHA-256 digests of the inputs actually read during that
build, including missing resolution candidates, the manifest, the lock and used cached package files.
Creating a missing import, deleting an input, editing CSS, or changing entries/the lock triggers a rebuild.
Unrelated files and generated outputs are not scanned. A pass is limited to 1,024 paths and 32 MiB of
input, read in 256 KiB chunks; each compilation has a 120 s deadline. Polling never overlaps a build or
another poll. Large projects can hit these explicit limits. An edit made during a build is detected by
the next poll; polling can miss a change that is reverted completely between reads.

Only a successful build and publication reload the preview, replacing its session history entry. A build
error leaves its last successful files and running document intact. Diagnostics (`file:line:column`, with
one-based line and column numbers) stream to the terminal and appear as a text overlay in the preview.
Fixing the source resumes rebuilding and clears the overlay. Ctrl+C ends with exit code 130, terminates
the Worker, cancels polling and aborts staged publication; a publication already committed finishes and reloads
the preview before stopping.
The last successful preview stays available. Browser disconnection also stops the loop.

`wf build`, `wf dev` and `wf test` are in developer distribution 0.6.0 and later; the default
version and integrity are selected by `DeveloperReleaseData.hpp`.

## Tests (`wf test`)

To test an existing application's own page from a consuming CMake project, see
[consumer Jasmine/CTest integration](CONSUMER_TESTING.md). That opt-in path is independent of
the developer terminal and does not require a wf manifest.

`wf test [--timeout <seconds>] [pattern]` runs the project's Jasmine specs in the browser. The project chooses
the Jasmine: `wf install jasmine-core` resolves it from the registry like any package, and the lock pins its version
and integrity. Nothing else is needed, and no public network once the package is in the cache.

```json
{
  "dependencies": {"jasmine-core": "7.0.2"},
  "test": {"specs": ["tests/model.spec.ts", "tests/view.spec.tsx"]}
}
```

- `test.specs` lists project paths, run in that order (JS, JSX, TS and TSX are built by esbuild-wasm in the Worker of
  `wf build`, with the locked dependencies). Specs use the Jasmine globals (`describe`, `it`, `expect`, `jasmine`);
  they do not import `jasmine-core`. A spec may import a stylesheet, which the runner page loads. Both the Jasmine 7 and the earlier `jasmine.js`, which only exposes `jasmineRequire`, are started. Without specs, or without `jasmine-core` in the lock, the command says what to
  declare or install.
- The specs run in a hidden frame of the environment's page (1024 × 768, same origin, with the bridge loaded, so a
  spec can call `webFront.cppFunction`). The runner page, Jasmine and everything the specs built (JS, CSS imported by a spec, assets) are published to a
  directory of the run's own, `.webfront/developer/test/<run id>/`, and served by the developer HTTP provider; two
  sessions on a project never run each other's specs. The files are removed when the run ends (an empty directory
  remains) and the frame is removed at the end, on a time limit and on Ctrl+C.
- `pattern` keeps the specs whose full name (suites and spec names) contains it; the others count as filtered out.
  The order is fixed: no random shuffling.
- The terminal shows `.`, `F` and `*` (passed, failed, pending) as specs end, then a summary line (`PASSED: 3 specs,
  0 failures, 1 pending (12 ms, jasmine-core 7.0.2)`), each failure with its message and first stack frames, failures
  outside any spec (a load error) and those of a suite (an `afterAll` that throws, shown as `FAIL <suite> (suite)`) and the pending specs with their reason.
- Exit codes: 0 when every executed spec passed; 1 for a failed spec, a failure outside the specs, an unhandled
  asynchronous exception or rejection (Jasmine reports both as failures), a spec file that does not build or load,
  `fit`/`fdescribe`, and a run that executed nothing (no specs, or a pattern matching none); 124 at the time limit
  (`--timeout`, default 60 s, 1 to 3600, covering the build and the publication as well as the run, and never later than the `--timeout` of a `wf run` script that runs `wf test`); 130 on Ctrl+C, at any step. Jasmine's own per-spec timeout (5 s by default) still ends a
  spec that never completes, which fails.
- A synchronous infinite loop in a spec blocks the page that hosts the frame, as it would in any same-origin frame:
  the time limit cannot fire. Use asynchronous waits, which the time limit and Ctrl+C do stop.

The result sent to C++ is cut to 60,000 bytes when it is larger (a bridge string holds 65,535): the output first, then stack frames, messages and the lists, never the counts or the status; `truncated` is then `true`.

The report is structured: `{status, exitCode, total, passed, failed, pending, excluded, durationMs, jasmine, failures:
[{name, messages: [{message, stack}]}], pendingSpecs, suites, global, incomplete}`, bounded (100 failures, 8 messages of 16 KiB
each per spec). `status` is `passed`, `failed`, `incomplete`, `error` (the specs did not load), `timeout` or `interrupted`.

### Without a terminal

For the optional native CLI/MCP controller of a live developer window, see
[developer automation](AUTOMATION.md). It reuses this command dispatch with explicit native
grants, structured diagnostics and watchdogs, without exporting scripts or application callbacks.

An application that drives the environment from its own page (`webfront-wf`, below, is one) calls
`runNonInteractive(argv, {bridge, config, preview, reporter, onExit})` from `runtime.mjs`:

```js
const runtime = await import('/webfront/developer/environment/runtime.mjs');
await runtime.runNonInteractive(['test'], {bridge: webFront, config, preview,
    reporter: json => webFront.cppFunction('app.result')(json)});
```

It runs the command, sends `{exitCode, output, report}` as JSON to the C++ function the application registered
(`app.result` here; the report is `null` for commands other than `wf test`) and then calls `onExit(exitCode)`, which
by default closes the window. A command that fails, or a result that cannot be sent, ends with a non-zero code and
the session is never left open. The C++ side reads the result, closes the application's window if it has not been
closed and returns the code as its process status.

### The test driver (`webfront-wf`) and the bootstrap environment

`webfront-wf` (`test/driver/WebFrontWf.cpp`, built with the browser tests) starts a `BasicDeveloperApp` on a project, runs
one `wf` command through `runNonInteractive`, prints its output and exits with its status:

```sh
webfront-wf --project <dir> [--environment <environment.tgz> --integrity <sha512-...>] [--registry-dir <dir>] \
            [--manifest-dir <dir>] [--report <file>] [--timeout <seconds>] [--command-timeout <seconds>] [--] <wf command> [arguments...]
```

- Without `--environment`, it uses the **bootstrap environment** provisioned at configure time. `cmake/WebFrontBootstrap.cmake`
  pins its URL and SHA-512 (distinct from `DeveloperReleaseData.hpp`, the environment applications start from, and changed
  by an explicit pull request). With `ENABLE_TESTING=ON`, configure downloads it to `<build>/bootstrap/` and verifies the
  digest; a wrong digest or an unreachable URL stops the configure. On a machine without network access, pass
  `-DWEBFRONT_BOOTSTRAP_DIR=<directory holding environment.tgz>`: the archive is verified the same way.
- `--environment` and `--integrity` run an archive built elsewhere, such as the one built from the current sources (the
  environment under test of #315). `--registry-dir` serves a local npm registry from the driver's origin and imposes it. `--manifest-dir` keeps the manifest and
  the lock in that directory of the project (usage error 64 when it is absolute, has an empty or dot component or leaves the project).
  The project is the directory `wf` reads and writes; run `wf install` once, then the other commands, on the same project.
- `--timeout` bounds the whole invocation, including browser/bootstrap startup. For a single command,
  `--command-timeout` optionally adds a shorter native watchdog starting after bootstrap, immediately before
  `runNonInteractive`; it never extends the overall deadline. Suites use their per-case budgets instead.
- Exit status: the command's own (`wf test`: 0, 1, 124, 130); **64** usage; **70** bootstrap failure (archive absent, wrong
  integrity, incompatible protocol or API, no non-interactive mode); **71** the driver's overall `--timeout` (default 600 s) or optional command deadline elapsed,
  after asking the page to close; **72** the browser closed without a result. `--report` writes the JSON result
  `{exitCode, output, report}` of the command, cut to fit a bridge string as described above.

`webtest`, the Jasmine bridge suite (`test/web/suite/`, #311), is this driver with the C++ callbacks of its specs
(`test/driver/WfDriver.hpp`: `run` and `main` take a setup that adds pages, registers callbacks before the page opens and
decides the final status). CTest copies the project, runs `wf install` from the registry that `wf run test-registry` built, then `webtest ... -- test`.

**Compatibility contract.** `environment.json` declares `protocol` (the environment's wire protocol) and `api` (the
`webfront.developer.*` callbacks it expects; absent means 1, the value of every environment published before the
field). `webfront.developer.config` carries `supported: {protocol: [1], api: [1]}`, from `developer::protocolVersion` and
`developer::apiVersion`. The loader refuses an archive outside those lists before publishing anything, with
`Incompatible WebFront developer environment: api version 2, this application supports 1`; it never falls back. A change
to the callbacks increments `apiVersion` and requires publishing a new bootstrap environment.

The release pin (`DeveloperReleaseData.hpp`) and test-bootstrap pin are independent;
`cmake/WebFrontBootstrap.cmake` keeps the verified 0.6.2 test bootstrap.
Historically, 0.6.0 was the first published environment with a non-interactive mode (`WfDriverPinned` runs `wf init` from it; the starters are not provisioned, only the archive), 0.6.1 fixed `runNonInteractive`, which dropped what a command wrote before it ended (a script's console output, install progress) and kept only its summary, and 0.6.2 reads the archives of `@types` packages and the manifest directory. The other `WfDriver*` cases run the environment built from the current sources
(the `SelfBuild` cases, assembled by the fixture `DeveloperFixtures`), which may be ahead of the pin: `wf install` and `wf test` on a fixture project (pass, fail,
report file), a starter, an entry module renamed in `environment.json`, an unknown command, the time limit, a wrong integrity,
a missing archive and archives with an incompatible protocol or API. `BootstrapProvisioning` checks the
configure-time provisioning (verified download, reuse, local directory, wrong digest, malformed value). The Linux CEF CI job
blocks the public network after the configure step.

## Dependencies, storage and limitations

- `wf install` accepts exact versions, registry tags and semver ranges, resolves ordinary
  transitive dependencies, and retains separate nodes for different versions. Required peers must
  be satisfied by compatible explicitly installed roots or direct dependencies; conflicts fail.
- Package installation scripts and native add-ons are not executed, and the APIs of server-side JavaScript runtimes are
  not available. Packages must run in a browser. Optional dependencies are not installed; imports of missing
  dependencies fail during the build. `wf` installs from npm registries and mirrors; it is not the complete npm client.
- Browser/production exports, ESM, CommonJS, TS/TSX, JSX, CSS and imported resource files are supported.
  Private package exports, network imports and unavailable Node built-ins fail. Package-specific
  build pipelines and automatic polyfills are not supplied.
- `webfront-packages.lock.json` records exact package versions, dependency edges, source integrities,
  requested roots and the digest of the manifest dependencies it resolved. Commit it and the source TSX/CSS; the generated provider can also be committed.
  Preview/embed consume the locked local files without resolving versions again.
- Unpacked packages are cached by name, version and integrity under `.webfront/developer/packages/`,
  each with a record of its files' digests. A failed install can leave unused cached packages, but the lock
  file remains unchanged. A lock naming more than 512 packages is refused.
- Published environment/preview resources are served under `/webfront/developer/`. The protected
  `/WebFront.js` remains the library's version. Sources, staging and cached archives are not HTTP
  routes. Project reads and publications reject traversal and escaping symlinks. This does not
  isolate downloaded tools or defend against concurrent modifications by another local process.
- Transfers use sequential 256 KiB chunks. Downloads are bounded to 64 MiB compressed; unpacking,
  dependency source graphs and individual publication transactions to 256 MiB, with 512 packages
  and eight active publications at most. A publication untouched for ten minutes (a reloaded or disconnected browser) is
  abandoned, and staging left by a crashed process is removed after an hour. Browser builds time out after 120 seconds.
- Installing another package keeps every locked version that still satisfies a requested range and fails if its
  archive no longer matches the recorded integrity; only new ranges are resolved. Review the resulting lock
  changes. `wf restore` rebuilds the cache of a lock on a fresh machine (see above).

## Environment distribution and offline tests (maintainers)

The environment builds itself (#315). The repository is a `wf` project, with its manifest and lock in `js/developer/` (the driver is given `--manifest-dir js/developer` and the project is the repository): `js/developer/webfront.project.json` declares the build
dependencies (esbuild-wasm, semver, xterm and its fit addon, and the React versions the starters pin), the scripts
and the permissions (`files` it reads, `publish` where it writes), and `js/developer/webfront-packages.lock.json` locks them by
SHA-512. The scripts of `js/developer/scripts/` run in the pinned bootstrap environment with the driver `webfront-wf`
(see [the test driver](#the-test-driver-webfront-wf-and-the-bootstrap-environment)). They prepare
a tarball that application developers can download. No command publishes anything to a registry automatically.

```sh
wf() { xvfb-run -a build-driver/test/web/webfront-wf --project "$PWD" -- "$@"; }   # the driver of the repository
wf restore                                          # the locked build dependencies
wf run build build-developer-environment --refresh-bootstrap
```

`js/developer/scripts/build.mjs` bundles the environment and compiler with the locked esbuild-wasm (run in the script's
Worker), includes esbuild.wasm, xterm styles and licenses, and writes deterministic `environment.tgz`, `life-starter.tgz`,
`provision.json` and `DeveloperReleaseData.hpp`. The version and public base URL are in
`js/developer/tools/distribution.json`. `--refresh-bootstrap` additionally refreshes the two checked-in JS bundles, their
input checksums (`bundles.json`, over `js/developer/tools/bundle-inputs.json`) and the pinned header; the core headers that embed
the bundles come from `cmake --build <dir> --target webfront-embedded-assets`. Without
that flag, it writes only to the output directory. `site.mjs` stages a build into the GitHub Pages site and `pin.mjs`
checks the pinned header against the published archive. The headers of the core's embedded assets are rendered by `js/core/EmbedAsset.cmake`, in pure CMake (see below). The archives have the package identifiers
`@webfront/developer-environment` and `@webfront/life-starter`. They are distributed as static
GitHub Pages downloads; no npm publication or scope ownership is required.

The `SelfBuild*` CTest cases run the scripts on a copy of the sources, with the dependencies installed from the local
registry: the build, a repeat build (same bytes), the checked-in bootstrap bundles and checksums rebuilt by wf alone (and the
core headers rendered from them by CMake), the staging of the site (a second staging repeats the same bytes, a changed
published file is refused) and the permissions of the manifest. `DeveloperSpecs` runs the pure JavaScript specs of
`test/developer/` with `wf test` on that copy; the CEF-off CTest cases check the generated headers (`*AssetFresh`,
`EmbeddedAssetGenerator`, and the Catch2 scenario on the embedded assets of the core), the checksums of the bundle inputs (`BundleInputsFresh`) and the names the
terminal and `webfront::Commands` reserve (`CommandNamesAgree`) with CMake and Catch2 alone, and `NoNodeDependency` audits
every file of the project. The maintainer procedure is in `js/developer/tools/PUBLISHING.md`.

To try the local archive without an HTTPS server, after building the packages above (with `wf run build build-developer-environment`):

```sh
mkdir -p life-project/.webfront/developer/preview/provision
cp build-developer-environment/*.tgz life-project/.webfront/developer/preview/provision/
integrity=$(sed -n 's/^  "integrity": "\(.*\)",$/\1/p' build-developer-environment/provision.json)
./build-native/examples/WebFrontLifeDeveloper "$PWD/life-project" \
  /webfront/developer/preview/provision/environment.tgz "$integrity"
```

The preview command later replaces that directory, but the verified archive is already cached
independently for subsequent launches. These are maintainer provisioning commands; the application's
terminal installs and builds browser dependencies without any local JavaScript tooling.

**The tests' local npm registry** is built by `wf run test-registry` (`test/web/registry/`): the packuments and tarballs as
plain files, from the real packages that CMake downloaded at configure time (`test/web/registry-packages.json` lists each by
name, version and SHA-512 integrity; a wrong digest stops the configure, `-DWEBFRONT_REGISTRY_DIR=<directory>` replaces the
downloads on a machine without network access) kept byte for byte, and from synthetic packages for the edge cases (a newer
version under a locked range, an archive that no longer matches its integrity, typed and untyped packages). The test
application serves it from its own origin and imposes it through the `registry` option. The CEF-off `DeveloperSpecs` case
generates the synthetic part in memory to test `wf restore`. No CTest case downloads from the public registry. Nothing is
committed: the archives live in the build directory.

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WKWEBVIEW=ON \
  -DWEBFRONT_WEBTEST_FRONTEND=WKWebView -DENABLE_TESTING=ON
cmake --build build-native --parallel
ctest --test-dir build-native -R 'Developer|LifeEmbedded' --output-on-failure
```

The CTest fixtures prepare the registry and the environment under test in every browser job (WKWebView, WebKitGTK, WebView2, CEF). The initial 0.2.0 Life version passed
WKWebView and all 14 Linux CEF/Xvfb cases with both GCC and Clang in
[run 39421265580](https://github.com/ambroise-leclerc/WebFront/actions/runs/39421265580).
The packed 0.3.0 version passes WKWebView and all 14 GCC CEF/Xvfb cases in
[run 39490581423](https://github.com/ambroise-leclerc/WebFront/actions/runs/39490581423).
For Linux, select CEF and Xvfb instead; the same developer tests use the selected test frontend.
`DeveloperBrowserIntegration` exercises the real WASM compiler, local registry, `wf test` under the locked jasmine-core (pass, fail, pending, asynchronous failures, pattern, time limit, Ctrl+C, non-interactive report to C++), build without navigation, incremental dev reload/error/recovery/Ctrl+C, the Life preview, C++ generation/animation and repeatable header generation. Its CTest fixtures then build that
**browser-generated** header and run `LifeEmbeddedIntegration` from an empty working directory
with no developer callbacks.
Deadline and grace-period regressions advance a scoped clock instead of sleeping for seconds; Worker startup,
bridge transfers and UI rendering still use the real browser. Dev polling is advanced explicitly after a committed
edit, and native command cancellation waits on a test-controlled release rather than an arbitrary linger duration.
Repeated type checks reuse immutable package bytes after the first real bridge read. After the Life application
checks, cache/command fixtures drop React DOM and retain a small real dependency graph, avoiding repeated transfers
of unrelated server/development bundles while preserving restore, integrity, compilation and rollback assertions. Retired PI and Signal Studio
application tests are removed; bridge scalars, arrays, exceptions and Promise rejection remain covered by the shared
bridge suite, and React/TSX/embedding by the maintained Life example. The browser suite remains one CTest case:
its total duration is reported without subdividing it into artificial timing units. Snapshot/NodeNext assertions
use the already installed real TypeScript compiler in the browser workflow. Developer specs, including cancellation
and clock regressions, run through the repository's own `wf test`. Two identical self-build invocations verify byte reproducibility; a separate build verifies bootstrap refresh.
The packaged-script permission rejection is also exercised end to end.

The standard CEF-off suite also checks project transactions, path rejection, Life rules and boundaries, MIME types,
bootstrap freshness, package resolution, gzip generation, interruption and offline cache reuse.

## Repository test sessions

The non-interactive test driver can run compatible commands in a shared browser session with fresh command frames and
individual JSON reports. The production `wf` command API is unchanged. See [TEST_PERFORMANCE](TEST_PERFORMANCE.md) for
`--suite`, isolated CTest filters, complete fixture repetition, quiet windows, profiling and concurrency constraints.
