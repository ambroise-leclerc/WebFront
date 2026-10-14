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

// Visible width of a string, ignoring ANSI escape sequences. Wide characters count as one column,
// which is enough for prompts and command lines in this first version.
function visibleLength(text) {
    return Array.from(text.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "")).length;
}

function commonPrefix(words) {
    return words.reduce((prefix, word) => {
        let length = 0;
        while (length < prefix.length && length < word.length && prefix[length] === word[length])
            ++length;
        return prefix.slice(0, length);
    });
}

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
        switch (data) {
            case "\r": return this.submit();
            case "\x7f": case "\b": return this.erase(-1);
            case "\x1b[3~": return this.erase(0);
            case "\x1b[D": return this.moveTo(this.cursor - 1);
            case "\x1b[C": return this.moveTo(this.cursor + 1);
            case "\x1b[H": case "\x1bOH": case "\x01": return this.moveTo(0);
            case "\x1b[F": case "\x1bOF": case "\x05": return this.moveTo(this.chars.length);
            case "\x1b[A": return this.recall(-1);
            case "\x1b[B": return this.recall(1);
            case "\t": return this.track(this.completeLine());
            case "\x03": return this.cancelLine();
            case "\x0c": return this.redraw(ansi.clearScreen);
            case "\x15": return this.killBeforeCursor();
        }
        if (data.startsWith("\x1b"))
            return;
        // Typed or pasted text: printable characters are inserted, a carriage return submits the
        // line and the rest of a multi-line paste is dropped, since commands run one at a time.
        const [text, ...rest] = data.split("\r");
        this.insert(Array.from(text).filter((char) => char >= " " && char !== "\x7f"));
        if (rest.length > 0)
            this.submit();
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
    const [prompt, banner] = await Promise.all([call("prompt")(), call("banner")()]);
    const editor = new LineEditor({write, run: call("run"), complete: call("complete"), prompt});
    if (banner)
        write(banner.replaceAll("\n", "\r\n") + "\r\n");
    editor.showPrompt();
    terminal.onData((data) => editor.handle(data));
    terminal.focus();
    return {terminal, editor, flush};
}
