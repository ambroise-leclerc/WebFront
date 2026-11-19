# Native pseudoterminals

Include `<webfront/Pty.hpp>` to run a program in a native terminal from C++. The child receives the requested argument vector, working directory, environment and initial terminal dimensions. Linux and macOS use a Unix PTY. Windows uses ConPTY, available since Windows 10 version 1809 and Windows Server 2019. [Microsoft's ConPTY documentation](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)

```cpp
webfront::Pty terminal({
    .argv = {"sh", "-i"},
    .directory = "/path/to/project",
    .environment = {{"PATH", "/usr/bin:/bin"}},
    .size = {100, 32}
});
```

Use UTF-8 strings for arguments and environment entries. The environment comes from `PtyOptions::environment`, with defaults for `TERM` (`xterm-256color`), `COLORTERM` (`truecolor`) and Windows `SystemRoot`. Supply any other variables the program needs, such as `HOME`, `USERPROFILE` or `PATH`. Relative paths are resolved against the child's directory. Bare executable names are searched in the supplied `PATH` or, when it is absent, in `/usr/bin:/bin` on Unix and the system directory on Windows. Windows also resolves `.cmd` and `.bat` shims and runs them through the system `cmd.exe`. Paths and arguments passed to batch scripts must omit double quotes, percent signs and line breaks.

- `write(bytes)` queues a whole chunk if there is room. It returns `false` immediately when the 64 KiB queue is full, the chunk exceeds capacity or the terminal is closing.
- `read(cursor, stopToken)` waits for data, exit, cancellation or hangup. `tryRead(cursor)` takes a snapshot without waiting for terminal I/O. Start at cursor zero and use the returned `next` for the following read. Each snapshot contains at most 16 KiB. `truncated` means the cursor fell behind the latest 1 MiB of retained output. The final snapshot carries the exit code.
- `resize({columns, rows})` schedules a resize on the terminal worker. Consecutive requests may be coalesced.
- `hangUp()` wakes readers and requests cleanup. `exitCode()` becomes available after cleanup. Destruction joins the worker. On Unix, cleanup signals the child and the foreground process group with SIGHUP and then escalates to SIGKILL. The host must leave the PTY child available for WebFront to reap. Cleanup also looks for remaining processes in the same session, including after the session leader exits. A process that creates a separate session may outlive it. On Windows, a job object terminates the entire process tree and ConPTY is closed.

After natural exit, the worker drains terminal output to EOF or until a three-second deadline when another process holds the terminal open. Unix waits on the terminal and a wake pipe, with a 250 ms backstop for child exit. Windows checks its output pipe every 40 ms while idle and every 10 ms when input is pending or final output is draining. macOS applies close-on-exec to the launch pipe immediately after creating it. A simultaneous host fork can briefly inherit it, in which case an unclosed writer can delay launch up to the ten-second startup deadline.

## Connect a browser page

Set `WebFrontConfig::allowPtyPages = true` and call `webfront::registerPtyPage(app, options)` from `<webfront/PtyPage.hpp>` before starting the server. Use a literal IPv4 loopback address or `::1` and set `contentPolicy.allowMissingOrigin = false`. Leave `allowAnyBridgeOrigin` disabled and `bridgeAllowedOrigins` empty. WebFront rejects incompatible configurations before binding. The application configures the executable, directory and environment. A page can open the configured session, send terminal input, resize, read output or hang up.

Every page of that origin shares access to the registered terminal. Serve trusted content and keep the loopback port private to the local application. A local native client can supply its own `Origin` header, and forwarding the port can extend access beyond the local machine. The terminal runs with the host application's privileges.

Bundle [the adapter](../js/pty/index.mjs) from the `@webfront/pty` package with your page and call `mount(element, {terminal, fit})` with your xterm.js `Terminal` and `FitAddon`. The adapter writes byte chunks to xterm so UTF-8 sequences can span reads. It forwards input and resizes the PTY when the container changes size. `onError` reports a rejected input or bridge failure, `onExit` receives the process status and `onTruncated` reports when some history has expired. The adapter rejects paste events that exceed the 64 KiB browser input queue, measured in UTF-8 bytes and reduced by pending input.

Each registration owns one session, opened on first attach and retained across page reloads, including after the child exits. Pass `restart: true` to `mount` to start a fresh session after the old process has finished. The old replay is discarded and pending calls for the old session are refused. A different callback name selects another application-configured session. Call `dispose()` when unmounting the adapter to stop its output requests and listeners. Call `hangUp()` to close the process. Application shutdown also closes the process. The adapter keeps one output request pending, for up to one second while idle. Reattachment replays the retained bytes, so after truncation a full-screen program may need to redraw.

Prepare a publishable `@webfront/pty` archive with `wf run pty-package` from the repository developer project. The example below uses the same adapter source directly.

## Run the example

The [standalone example](../examples/pty/) opens the system shell in the application's working directory. Run the following commands from the repository root in a POSIX shell with the WebFront developer environment available for the `wf` commands.

```sh
cp js/pty/index.mjs examples/pty/terminal.mjs
cd examples/pty
wf restore
wf build
cd ../..
cmake -S examples/pty -B build-pty-example -DWEBFRONT_AUTODETECT_NATIVE=OFF
cmake --build build-pty-example
./build-pty-example/WebFrontPtyExample examples/pty/.webfront/developer/build
```

On Windows, pass the same build output directory to `WebFrontPtyExample.exe` in the chosen CMake configuration's output directory.
