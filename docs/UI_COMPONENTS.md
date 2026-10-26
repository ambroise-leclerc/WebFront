# UI components

Status: removed (#313, delivered with #308). The opt-in terminal component (`webfront::Terminal`,
`fs::TerminalFS`, #230) and the `wf` tool manager (`webfront::ToolManager`, `fs::ToolStore`,
`fs::PackageFS`, `fs::ToolManagerFS`, #234) are gone, with their embedded xterm.js copy. Their design
notes remain in the Git history of this file.

They are replaced by the developer environment, downloaded and verified by
`NativeApp::bootstrap("developer")` ([DEVELOPER_ENVIRONMENT](DEVELOPER_ENVIRONMENT.md)):

| Removed | Replacement |
| --- | --- |
| `webfront::Terminal` page and its line editor | The environment's terminal: streaming, Ctrl+C for every command, per-browser sessions, multi-row redraw, CJK and emoji widths, history, completion |
| `terminal::Shell` and `Terminal::command` | `webfront::Commands` (`<webfront/Commands.hpp>`): coroutine commands, discovered by the environment |
| `Terminal::extension` browser commands | `wf run` scripts and `wf exec` package entries |
| `ToolManager`, `wf install/list/remove/open` of tools | `wf install` and `wf restore` of browser packages, locked by `webfront-packages.lock.json` |

## Migrating `terminal::Shell` commands

A command of the removed component returned its output once it ended, on the server thread:

```cpp
terminal.command("greet", "Greet someone", [](webfront::CommandArgs args) {
    return "Hello, " + (args.empty() ? std::string{"world"} : args[0]) + "!";
});
```

With `webfront::Commands`, it is a coroutine that streams its output and can be stopped:

```cpp
webfront::Commands commands{app};
commands.add("greet", "Greet someone", [](webfront::Commands<webfront::NativeApp>::Context context) -> webfront::Task {
    context.write("Hello, " + (context.args().empty() ? std::string{"world"} : context.args()[0]) + "!\n");
    co_return;
});
```

- `CommandArgs` becomes `context.args()`; the `std::ostream&` output becomes `context.write(text)`, or
  `co_await context.print(text)` for large output; `"\n"` ends a line.
- An exception is still shown as an error; `context.exit(code)` sets a non-zero exit code.
- A command must not block the server thread: long work awaits senders (`context.ui().resumeAfter`,
  `jsFunctionSender`) or runs on another thread. Ctrl+C, the browser's disconnection and shutdown stop it.
- `help`, `clear`, `history` and `exit` are built into the terminal; their names are reserved.
