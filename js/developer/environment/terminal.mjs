// The environment's line editor: multi-row redraw, grapheme clusters and their cell widths, history, multi-line paste,
// completion and Ctrl+C. The cell widths are also given to xterm.js (unicodeProvider), so both agree on every row.

const ansi = {
    red: text => `\x1b[31m${text}\x1b[0m`,
    up: count => count > 0 ? `\x1b[${count}A` : '',
    right: count => count > 0 ? `\x1b[${count}C` : '',
    clearBelow: '\x1b[J',
    clearScreen: '\x1b[2J\x1b[3J\x1b[H',
};
export const clearScreen = ansi.clearScreen;

// --- Cell widths -----------------------------------------------------------------------------------------------

const zeroWidth = /^[\p{Mn}\p{Me}\p{Cf}]$/u;
const presentation = /^\p{Emoji_Presentation}$/u;
const pictographic = /^\p{Extended_Pictographic}$/u;
// East Asian Wide and Fullwidth characters outside the emoji, which \p{Emoji_Presentation} covers.
const wide = [[0x1100, 0x115f], [0x2e80, 0x303e], [0x3041, 0x33ff], [0x3400, 0x4dbf], [0x4e00, 0x9fff], [0xa000, 0xa4cf],
    [0xa960, 0xa97f], [0xac00, 0xd7a3], [0xf900, 0xfaff], [0xfe10, 0xfe19], [0xfe30, 0xfe6f], [0xff00, 0xff60],
    [0xffe0, 0xffe6], [0x16fe0, 0x16fe4], [0x17000, 0x18cff], [0x1b000, 0x1b2ff], [0x1f200, 0x1f2ff], [0x20000, 0x2fffd],
    [0x30000, 0x3fffd]];

/** The columns a code point takes on its own: 0 (combining, format and control), 1 or 2 (wide and emoji). */
export function wcwidth(code) {
    if (code < 0x20 || (code >= 0x7f && code < 0xa0)) return 0;
    if (code < 0x300) return 1;
    const char = String.fromCodePoint(code);
    if (zeroWidth.test(char) || (code >= 0x1160 && code <= 0x11ff)) return 0;
    if (presentation.test(char) || wide.some(([first, last]) => code >= first && code <= last)) return 2;
    return 1;
}

// The join state carried from a code point to the next, in xterm.js's property value.
const State = {none: 0, joiner: 1, regionalFirst: 2, regionalSecond: 3, pictographic: 4};
const joiner = 0x200d;
const emojiStyle = 0xfe0f;
const isRegional = code => code >= 0x1f1e6 && code <= 0x1f1ff;
const isModifier = code => code >= 0x1f3fb && code <= 0x1f3ff;

function stateOf(code, previous) {
    if (code === joiner) return State.joiner;
    if (isRegional(code)) return previous === State.regionalFirst ? State.regionalSecond : State.regionalFirst;
    return pictographic.test(String.fromCodePoint(code)) ? State.pictographic : State.none;
}

function joins(code, width, state, previous) {
    if (width === 0) return true;
    if (previous === State.joiner) return state === State.pictographic;
    if (isRegional(code)) return previous === State.regionalFirst;
    return isModifier(code) && previous === State.pictographic;
}

/**
 * xterm.js's character properties: whether a code point joins the cell before it (combining marks, emoji ZWJ sequences,
 * flags, skin tones, emoji presentation selector) and the width of the resulting cell. The value packs the join state,
 * the width and the join flag like xterm.js's UnicodeService.createPropertyValue.
 */
export function charProperties(code, preceding) {
    let width = wcwidth(code);
    const previousState = preceding >>> 3, previousWidth = (preceding >> 1) & 3;
    let state = stateOf(code, previousState);
    const join = preceding !== 0 && previousWidth !== 0 && joins(code, width, state, previousState);
    if (join) {
        if (code === emojiStyle && previousState === State.pictographic) width = 2;
        width = Math.max(width, previousWidth);
        if (state === State.none) state = previousState;
    }
    return (state << 3) | (width << 1) | (join ? 1 : 0);
}

/** The Unicode provider registered in xterm.js, so that the terminal lays out cells like the editor. */
export const unicodeProvider = {version: 'webfront-1', wcwidth, charProperties};

/** Splits text into the cells xterm.js draws: grapheme clusters with their width, after the given join state. */
export function cells(text, preceding = 0) {
    const clusters = [];
    for (const char of text) {
        const properties = charProperties(char.codePointAt(0), preceding);
        const width = (properties >> 1) & 3;
        if ((properties & 1) && clusters.length) Object.assign(clusters.at(-1), {text: clusters.at(-1).text + char, width});
        else clusters.push({text: char, width});
        preceding = properties;
    }
    return {clusters, preceding};
}

/**
 * Where each cell starts, in rows and columns from the start of the prompt, and where the cursor is after the last one.
 * A wide cell that does not fit the end of a row starts the next one, like in xterm.js.
 */
export function layout(clusters, columns) {
    let row = 0, column = 0;
    const starts = [];
    for (const {width} of clusters) {
        if (width > 0 && column + width > columns) {
            row++;
            column = 0;
        }
        starts.push({row, column});
        column += width;
    }
    const end = column >= columns ? {row: row + 1, column: 0} : {row, column};
    return {starts, end};
}

// --- Words -----------------------------------------------------------------------------------------------------

// Reads a line for tokenize(), one word part at a time.
class Scanner {
    constructor(line) {
        this.line = line;
        this.index = 0;
    }
    get done() { return this.index === this.line.length; }
    next(problem) {
        if (this.done) throw new SyntaxError(problem);
        return this.line[this.index++];
    }
    // The content of a quoted string whose opening quote was read. Inside double quotes a backslash escapes the next
    // character; one ending the line stays literal, and the missing closing quote is reported next.
    quoted(quote) {
        let content = '';
        for (let char = this.next('Unterminated quote'); char !== quote; char = this.next('Unterminated quote'))
            content += quote === '"' && char === '\\' && !this.done ? this.next() : char;
        return content;
    }
    part(char) {
        if (char === '\\') return this.next('Trailing backslash');
        if (char === "'" || char === '"') return this.quoted(char);
        return char;
    }
}

/**
 * Splits a line into words: whitespace separates words, single quotes keep their content verbatim, double quotes keep
 * it but honor backslash escapes, and an unquoted backslash escapes the next character. Throws a SyntaxError on an
 * unterminated quote or a trailing backslash.
 */
export function tokenize(line) {
    const words = [];
    const scanner = new Scanner(line);
    let word = null;  // null between words
    while (!scanner.done) {
        const char = scanner.next();
        const separator = char === ' ' || char === '\t';
        if (separator && word !== null) words.push(word);
        word = separator ? null : (word ?? '') + scanner.part(char);
    }
    if (word !== null) words.push(word);
    return words;
}

/** A word as typed on a line: whitespace, quotes and backslashes escaped. */
export const escapeWord = word => word.replace(/[\s'"\\]/g, '\\$&');

function commonPrefix(words) {
    return words.reduce((prefix, word) => {
        let length = 0;
        while (length < Math.min(prefix.length, word.length) && prefix[length] === word[length]) ++length;
        return prefix.slice(0, length);
    });
}

// --- Line editor -----------------------------------------------------------------------------------------------

// Editing keys, as xterm.js reports them in onData, mapped to LineEditor actions.
const keyActions = new Map([
    ['\r', editor => editor.submit()],
    ['\x7f', editor => editor.erase(-1)],
    ['\b', editor => editor.erase(-1)],
    ['\x1b[3~', editor => editor.erase(0)],
    ['\x1b[D', editor => editor.moveTo(editor.cursor - 1)],
    ['\x1b[C', editor => editor.moveTo(editor.cursor + 1)],
    ['\x1b[H', editor => editor.moveTo(0)],
    ['\x1bOH', editor => editor.moveTo(0)],
    ['\x01', editor => editor.moveTo(0)],
    ['\x1b[F', editor => editor.moveTo(editor.cells.length)],
    ['\x1bOF', editor => editor.moveTo(editor.cells.length)],
    ['\x05', editor => editor.moveTo(editor.cells.length)],
    ['\x1b[A', editor => editor.recall(-1)],
    ['\x1b[B', editor => editor.recall(1)],
    ['\t', editor => editor.track(editor.completeLine())],
    ['\x03', editor => editor.cancelLine()],
    ['\x0c', editor => editor.clearScreen()],
    ['\x15', editor => editor.killBeforeCursor()],
    ['\x0b', editor => editor.killAfterCursor()],
]);

export const historyLimit = 500;

/**
 * Line editor driven by the terminal's input data.
 *   write(text): displays text; columns(): the terminal's width;
 *   run(line, signal): a Promise of the command's output; it rejects with an Error whose exitCode (1 by default, 130
 *     once Ctrl+C aborted `signal`) becomes the status;
 *   complete(line): a Promise of {word, candidates}, the word being completed at the end of the line and the full words
 *     that can replace it;
 *   saveHistory(lines): persists the history after each command.
 * Commands run one at a time; lines pasted together run one after the other.
 */
export class LineEditor {
    constructor({write, columns = () => 80, run, complete = async () => ({word: '', candidates: []}), prompt = '> ', history = [], saveHistory = () => {}}) {
        Object.assign(this, {write, columns, run, complete, saveHistory});
        this.setPrompt(prompt);
        this.cells = [];
        this.cursor = 0;
        this.row = 0;  // the cursor's row below the prompt's first row
        this.history = history.slice(-historyLimit);
        this.historyIndex = this.history.length;
        this.queue = [];  // pasted lines waiting for the running command: {text, submit}
        this.status = 0;
        this.abort = null;
        this.busy = null;
        this.closed = false;
    }

    get line() { return this.text(0); }

    setPrompt(prompt) {
        this.prompt = prompt;
        ({clusters: this.promptCells, preceding: this.promptState} = cells(prompt));
    }

    /** Resolves once no command, queued line or completion is pending. */
    async whenIdle() {
        while (this.busy) await this.busy;
    }

    showPrompt() {
        this.row = 0;
        this.write(this.prompt);
        if (this.cells.length) this.redraw();
    }

    handle(data) {
        if (this.closed) return;
        if (this.busy) return this.whileBusy(data);
        const action = keyActions.get(data);
        if (action) return action(this);
        if (!data.startsWith('\x1b')) return this.insertText(data);
    }

    // While a command runs, Ctrl+C aborts it and drops the pasted lines waiting for it; pasted lines wait their turn.
    whileBusy(data) {
        if (data === '\x03') return this.interrupt();
        if (/[\r\n]/.test(data)) this.enqueue(data.split(/\r\n?|\n/));
    }

    interrupt() {
        this.queue = [];
        if (this.abort?.signal.aborted !== false) return;
        this.write('^C\r\n');
        this.abort.abort(new Error('Interrupted'));
    }

    enqueue([...segments]) {
        const last = segments.pop();
        for (const text of segments) this.queue.push({text, submit: true});
        if (last) this.queue.push({text: last, submit: false});
    }

    // Typed or pasted text: printable characters are inserted; each line break submits the line, and the lines after
    // the first wait for the command before them.
    insertText(data) {
        const [first, ...rest] = data.split(/\r\n?|\n/);
        this.insert(first);
        if (!rest.length) return;
        this.enqueue(rest);
        return this.submit();
    }

    text(from, to) {
        return this.cells.slice(from, to).map(cell => cell.text).join('');
    }

    // Sets the line to `before` + `after`, the cursor between them. The whole line is segmented again: an inserted or
    // erased mark or joiner changes the cells around it.
    edit(before, after) {
        this.cells = cells(before + after, this.promptState).clusters;
        this.cursor = cells(before, this.promptState).clusters.length;
        this.redraw();
    }

    insert(text) {
        const printable = Array.from(text).filter(char => char >= ' ' && char !== '\x7f').join('');
        if (printable) this.edit(this.text(0, this.cursor) + printable, this.text(this.cursor));
    }

    setLine(text) {
        this.edit(text, '');
    }

    erase(offset) {
        const index = this.cursor + offset;
        if (index >= 0 && index < this.cells.length) this.edit(this.text(0, index), this.text(index + 1));
    }

    killBeforeCursor() {
        this.edit('', this.text(this.cursor));
    }

    killAfterCursor() {
        this.edit(this.text(0, this.cursor), '');
    }

    moveTo(position) {
        const cursor = Math.max(0, Math.min(this.cells.length, position));
        if (cursor === this.cursor) return;
        this.cursor = cursor;
        this.redraw();
    }

    recall(direction) {
        const index = this.historyIndex + direction;
        if (index < 0 || index > this.history.length) return;
        this.historyIndex = index;
        this.setLine(index === this.history.length ? '' : this.history[index]);
    }

    cancelLine() {
        this.moveTo(this.cells.length);
        this.write('^C\r\n');
        this.cells = [];
        this.cursor = 0;
        this.historyIndex = this.history.length;
        this.showPrompt();
    }

    clearScreen() {
        this.write(ansi.clearScreen);
        this.showPrompt();
    }

    // Rewrites the prompt and the line from the prompt's first row, then places the cursor. A line that exactly fills
    // its last row leaves xterm.js's cursor waiting to wrap: a line break moves it where the layout expects it.
    redraw() {
        const columns = Math.max(1, this.columns());
        const all = [...this.promptCells, ...this.cells];
        const {starts, end} = layout(all, columns);
        const cursor = this.cursor === this.cells.length ? end : starts[this.promptCells.length + this.cursor];
        let text = ansi.up(this.row) + '\r' + ansi.clearBelow + this.prompt + this.line;
        if (end.column === 0 && end.row > 0) text += '\r\n';
        text += ansi.up(end.row - cursor.row) + '\r' + ansi.right(cursor.column);
        this.row = cursor.row;
        this.write(text);
    }

    /** After a resize, xterm.js reflows the rows: the cursor keeps its place in the line, on a recomputed row. */
    resized() {
        const columns = Math.max(1, this.columns());
        const {starts, end} = layout([...this.promptCells, ...this.cells], columns);
        this.row = (this.cursor === this.cells.length ? end : starts[this.promptCells.length + this.cursor]).row;
    }

    track(promise) {
        this.busy = promise.finally(() => { this.busy = null; });
        return this.busy;
    }

    remember(line) {
        if (line.trim() === '' || this.history.at(-1) === line) return;
        this.history.push(line);
        if (this.history.length > historyLimit) this.history.splice(0, this.history.length - historyLimit);
        try { this.saveHistory(this.history.slice()); } catch { /* the history is a convenience */ }
    }

    // Moves below the line, runs it, then the pasted lines queued after it.
    submit() {
        this.moveTo(this.cells.length);
        const line = this.line;
        this.write('\r\n');
        this.cells = [];
        this.cursor = 0;
        this.row = 0;
        return this.track(this.runLines(line));
    }

    async runLines(line) {
        for (;;) {
            this.remember(line);
            this.historyIndex = this.history.length;
            await this.execute(line);
            if (this.closed) return;
            this.showPrompt();
            const next = this.queue.shift();
            if (!next) return;
            this.setLine(next.text);
            if (!next.submit) return;
            this.write('\r\n');
            line = next.text;
            this.cells = [];
            this.cursor = 0;
            this.row = 0;
        }
    }

    async execute(line) {
        if (line.trim() === '') return;
        this.abort = new AbortController();
        const {signal} = this.abort;
        try {
            const output = await this.run(line, signal);
            this.status = 0;
            // The prompt starts on a new line, unless the output already ends one or leaves the cursor at home.
            if (output) this.write(output.endsWith('\n') || output.endsWith('\x1b[H') ? output : output + '\r\n');
        } catch (error) {
            this.status = error?.exitCode ?? (signal.aborted ? 130 : 1);
            const message = String(error?.message ?? error);
            // Ctrl+C already shows ^C: a bare interruption adds nothing.
            if (!(signal.aborted && message === 'Interrupted'))
                this.write(ansi.red(`Error: ${message}${this.status !== 1 ? ` (exit code ${this.status})` : ''}`).replace(/(?<!\r)\n/g, '\r\n') + '\r\n');
        } finally {
            this.abort = null;
        }
    }

    // Completes the word before the cursor, at the end of the line: one candidate is inserted whole, several by their
    // common prefix, or listed when they share nothing more.
    async completeLine() {
        if (this.cursor !== this.cells.length) return;
        const line = this.line;
        let word, candidates;
        try { ({word, candidates} = await this.complete(line)); }
        catch { return; }
        if (!candidates?.length || this.line !== line) return;
        if (candidates.length === 1) {
            const [candidate] = candidates;
            return this.insert(escapeWord(candidate.slice(word.length)) + (candidate.endsWith('/') ? '' : ' '));
        }
        const prefix = commonPrefix(candidates);
        if (prefix.length > word.length) return this.insert(escapeWord(prefix.slice(word.length)));
        this.moveTo(this.cells.length);
        this.write('\r\n' + candidates.join('  ') + '\r\n');
        this.showPrompt();
    }

    /** Ends the session: the editor no longer reads input. */
    close() {
        this.closed = true;
        this.queue = [];
    }
}
