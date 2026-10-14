/// @brief WebFront terminal component: an xterm.js terminal whose line editor sends each validated
/// line to the C++ commands registered through webfront::Terminal (include/components/Terminal.hpp).

import {Terminal} from "./xterm.mjs";
import {FitAddon} from "./addon-fit.mjs";

const ansi = {
    red: (text) => `\x1b[31m${text}\x1b[0m`,
    clearToEnd: "\x1b[K",
    clearScreen: "\x1b[2J\x1b[3J\x1b[H",
    left: (count) => (count > 0 ? `\x1b[${count}D` : ""),
};

function commonPrefix(words) {
    return words.reduce((prefix, word) => {
        const limit = Math.min(prefix.length, word.length);
        let length = 0;
        while (length < limit && prefix[length] === word[length])
            ++length;
        return prefix.slice(0, length);
    });
}

// Editing keys, as xterm.js reports them in onData, mapped to LineEditor actions.
const keyActions = new Map([
    ["\r", (editor) => editor.submit()],
    ["\x7f", (editor) => editor.erase(-1)],
    ["\b", (editor) => editor.erase(-1)],
    ["\x1b[3~", (editor) => editor.erase(0)],
    ["\x1b[D", (editor) => editor.moveTo(editor.cursor - 1)],
    ["\x1b[C", (editor) => editor.moveTo(editor.cursor + 1)],
    ["\x1b[H", (editor) => editor.moveTo(0)],
    ["\x1bOH", (editor) => editor.moveTo(0)],
    ["\x01", (editor) => editor.moveTo(0)],
    ["\x1b[F", (editor) => editor.moveTo(editor.chars.length)],
    ["\x1bOF", (editor) => editor.moveTo(editor.chars.length)],
    ["\x05", (editor) => editor.moveTo(editor.chars.length)],
    ["\x1b[A", (editor) => editor.recall(-1)],
    ["\x1b[B", (editor) => editor.recall(1)],
    ["\t", (editor) => editor.track(editor.completeLine())],
    ["\x03", (editor) => editor.cancelLine()],
    ["\x0c", (editor) => editor.redraw(ansi.clearScreen)],
    ["\x15", (editor) => editor.killBeforeCursor()],
]);

/// Line editor driven by the terminal's input data. `write(text)` displays text; `run(line)` and
/// `complete(line)` return Promises of the C++ output and of "\n"-separated completion candidates.
export class LineEditor {
    constructor({write, run, complete, prompt}) {
        this.write = write;
        this.run = run;
        this.complete = complete;
        this.prompt = prompt;
        this.chars = [];
        this.cursor = 0;
        this.history = [];
        this.historyIndex = 0;
        this.busy = null;
    }

    get line() {
        return this.chars.join("");
    }

    /// Resolves once no command or completion is pending.
    async whenIdle() {
        while (this.busy)
            await this.busy;
    }

    showPrompt() {
        this.write(this.prompt);
    }

    handle(data) {
        if (this.busy)
            return;
        const action = keyActions.get(data);
        if (action)
            return action(this);
        if (!data.startsWith("\x1b"))
            return this.insertText(data);
    }

    // Typed or pasted text: printable characters are inserted, a carriage return submits the line
    // and the rest of a multi-line paste is dropped, since commands run one at a time.
    insertText(data) {
        const [text, ...rest] = data.split("\r");
        this.insert(Array.from(text).filter((char) => char >= " " && char !== "\x7f"));
        if (rest.length > 0)
            return this.submit();
    }

    insert(chars) {
        if (chars.length === 0)
            return;
        this.chars.splice(this.cursor, 0, ...chars);
        this.cursor += chars.length;
        this.redraw();
    }

    erase(offset) {
        const index = this.cursor + offset;
        if (index < 0 || index >= this.chars.length)
            return;
        this.chars.splice(index, 1);
        this.cursor = index;
        this.redraw();
    }

    killBeforeCursor() {
        this.chars.splice(0, this.cursor);
        this.cursor = 0;
        this.redraw();
    }

    moveTo(position) {
        const cursor = Math.max(0, Math.min(this.chars.length, position));
        if (cursor === this.cursor)
            return;
        this.cursor = cursor;
        this.redraw();
    }

    recall(direction) {
        const index = this.historyIndex + direction;
        if (index < 0 || index > this.history.length)
            return;
        this.historyIndex = index;
        this.chars = Array.from(index === this.history.length ? "" : this.history[index]);
        this.cursor = this.chars.length;
        this.redraw();
    }

    cancelLine() {
        this.write("^C\r\n");
        this.chars = [];
        this.cursor = 0;
        this.historyIndex = this.history.length;
        this.showPrompt();
    }

    // Rewrites the prompt and the line in place, then moves the cursor back to its position.
    redraw(prefix = "\r") {
        this.write(prefix + this.prompt + this.line + ansi.clearToEnd + ansi.left(this.chars.length - this.cursor));
    }

    track(promise) {
        this.busy = promise.finally(() => {
            this.busy = null;
        });
        return this.busy;
    }

    submit() {
        const line = this.line;
        this.write("\r\n");
        this.chars = [];
        this.cursor = 0;
        if (line.trim() !== "" && this.history.at(-1) !== line)
            this.history.push(line);
        this.historyIndex = this.history.length;
        return this.track(this.execute(line));
    }

    async execute(line) {
        try {
            const output = await this.run(line);
            // The prompt starts on a new line, unless the output already ends one or leaves the
            // cursor at home (such as the 'clear' command).
            if (output)
                this.write(output.endsWith("\n") || output.endsWith("\x1b[H") ? output : output + "\r\n");
        } catch (error) {
            this.write(ansi.red(`Error: ${error?.message ?? error}`) + "\r\n");
        }
        this.showPrompt();
    }

    async completeLine() {
        // Only the command name is completed, and only with the cursor at the end of the line.
        if (this.cursor !== this.chars.length)
            return;
        const line = this.line;
        let candidates;
        try {
            candidates = (await this.complete(line)).split("\n").filter((word) => word !== "");
        } catch (error) {
            return;
        }
        if (candidates.length === 0)
            return;
        const completion = candidates.length === 1 ? candidates[0] + " " : commonPrefix(candidates);
        if (completion.length > line.length)
            return this.insert(Array.from(completion.slice(line.length)));
        this.write("\r\n" + candidates.join("  ") + "\r\n");
        this.redraw();
    }
}

/// Mounts a terminal in `element` once the WebFront bridge is ready. Returns the xterm Terminal,
/// its line editor and a `flush()` resolving when every write has been rendered.
export async function mount(element, {bridge = globalThis.webFront, terminalOptions = {}} = {}) {
    const terminal = new Terminal({
        cursorBlink: true,
        convertEol: false,
        fontFamily: 'ui-monospace, "Cascadia Code", "Source Code Pro", Menlo, Consolas, monospace',
        theme: {background: "#0d1117", foreground: "#e6edf3"},
        ...terminalOptions,
    });
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    terminal.open(element);
    fit.fit();
    new ResizeObserver(() => fit.fit()).observe(element);

    let lastWrite = Promise.resolve();
    const write = (text) => {
        lastWrite = new Promise((resolve) => terminal.write(text, resolve));
    };
    const flush = () => lastWrite;

    try {
        await bridge.ready;
    } catch (error) {
        write(ansi.red(`Connection to the application failed: ${error?.message ?? error}`) + "\r\n");
        return {terminal, editor: null, flush};
    }

    const call = (name) => bridge.cppFunction(`webfront.terminal.${name}`);
    let prompt, banner;
    try {
        [prompt, banner] = await Promise.all([call("prompt")(), call("banner")()]);
    } catch (error) {
        // For example a page served by an application without webfront::Terminal.
        write(ansi.red(`Terminal unavailable: ${error?.message ?? error}`) + "\r\n");
        return {terminal, editor: null, flush};
    }
    const editor = new LineEditor({write, run: call("run"), complete: call("complete"), prompt});
    if (banner)
        write(banner.replaceAll("\n", "\r\n") + "\r\n");
    editor.showPrompt();
    terminal.onData((data) => editor.handle(data));
    terminal.focus();
    return {terminal, editor, flush};
}
