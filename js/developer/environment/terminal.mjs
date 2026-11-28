// The environment's line editor: multi-row redraw, grapheme clusters and their cell widths, history, multi-line paste,
// completion, the interruption of a command and the system's shortcuts (copy, paste, cut, select all). The cell widths are
// also given to xterm.js (unicodeProvider), so both agree on every row.

const ansi = {
    red: text => `\x1b[31m${text}\x1b[0m`,
    dim: text => `\x1b[2m${text}\x1b[0m`,
    up: count => count > 0 ? `\x1b[${count}A` : '',
    right: count => count > 0 ? `\x1b[${count}C` : '',
    clearBelow: '\x1b[J',
    clearScreen: '\x1b[2J\x1b[3J\x1b[H',
};
export const clearScreen = ansi.clearScreen;

// --- Untrusted text ----------------------------------------------------------------------------------------------

// C0 controls, DEL and the C1 controls (U+0080 to U+009F, where U+009B is an 8-bit CSI) would drive the terminal.
const visibleControl = char => {
    const code = char.charCodeAt(0);
    if (code === 0x7f) return '^?';
    return code < 0x20 ? '^' + String.fromCharCode(code + 64) : '\\x' + code.toString(16);
};

/**
 * Text of a project file for the terminal: its control characters are shown (^[ for ESC, \x9b for a C1 control) instead
 * of being sent; tabs and line breaks stay, and "\r\n" is one line break.
 */
export const printable = text => String(text).replace(/\r\n/g, '\n').replace(/[\x00-\x08\x0b-\x1f\x7f-\x9f]/g, visibleControl);

/** A name or a value that comes from the project, its lock or a package, on one line: every control character is shown. */
export const inline = text => String(text).replace(/[\x00-\x1f\x7f-\x9f]/g, visibleControl);

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

/**
 * The word being typed at the end of a line, read like tokenize(): where it starts, its value and the quote it leaves
 * open ("'", '"' or null). Null after a backslash that escapes nothing yet.
 */
export function wordAtEnd(line) {
    let start = 0, quote = null, escaped = false;
    for (let index = 0; index < line.length; ++index) {
        const char = line[index];
        if (escaped) escaped = false;
        else if (quote) {
            if (char === quote) quote = null;
            else if (char === '\\' && quote === '"') escaped = true;
        }
        else if (char === '\\') escaped = true;
        else if (char === "'" || char === '"') quote = char;
        else if (char === ' ' || char === '\t') start = index + 1;
    }
    if (escaped) return null;
    const typed = line.slice(start);
    return {start, quote, value: typed ? tokenize(typed + (quote ?? ''))[0] : ''};
}

/** Text added to a word, escaped for the quote left open in it; null when it cannot be written there. */
export function escapeFor(text, quote) {
    if (quote === "'") return text.includes("'") ? null : text;
    if (quote === '"') return text.replace(/["\\]/g, '\\$&');
    return escapeWord(text);
}

function commonPrefix(words) {
    return words.reduce((prefix, word) => {
        let length = 0;
        while (length < Math.min(prefix.length, word.length) && prefix[length] === word[length]) ++length;
        return prefix.slice(0, length);
    });
}

// --- System shortcuts ------------------------------------------------------------------------------------------

/** What Escape sends in onData: the key that interrupts a command while no text of the terminal is selected. */
export const interruptKey = '\x1b';

const shortcutLetters = {c: 'copy', v: 'paste', x: 'cut', a: 'selectAll'};
const shortcutCodes = {KeyC: 'copy', KeyV: 'paste', KeyX: 'cut', KeyA: 'selectAll'};

/**
 * The system shortcut that a key event of the terminal is, or null when it is a key for the line editor:
 *   copy, paste, cut, selectAll: Ctrl or ⌘ (Meta) with C, V, X or A, Shift allowed (Ctrl+Shift+C as in other terminals)
 *     but not Alt, which AltGr sets with Ctrl on Windows. The letter is the one the layout gives (Ctrl+A on AZERTY is
 *     the key labelled A), or the key's place for a layout without Latin letters, as the system does;
 *   clearSelection: Escape alone while text of the terminal is selected; without a selection, Escape goes to the editor
 *     (interruptKey).
 */
export function shortcutOf(event, hasSelection) {
    const bare = !event.ctrlKey && !event.altKey && !event.metaKey && !event.shiftKey;
    if (event.key === 'Escape') return bare && hasSelection ? 'clearSelection' : null;
    if (!(event.ctrlKey || event.metaKey) || event.altKey) return null;
    const key = String(event.key ?? '').toLowerCase();
    return /^[a-z]$/.test(key) ? shortcutLetters[key] ?? null : shortcutCodes[event.code] ?? null;
}

/**
 * Lets the system's shortcuts work in an xterm.js `terminal`, instead of becoming control characters for the editor
 * (Ctrl+C would send \x03, Ctrl+V \x16): the engine runs its own copy, cut and paste, whichever the frontend (CEF,
 * WebView2, WebKitGTK, WKWebView through its Edit menu, a system browser), and xterm.js answers them:
 *   - copy and cut put the selection on the clipboard; nothing of the terminal can be removed, so a cut copies. The
 *     selection is also put in xterm.js's hidden textarea and selected there, as xterm.js does for its context menu,
 *     so that an engine that enables Copy only for a selection of the page (a native Edit menu) enables it;
 *   - paste reaches xterm.js's paste handler, which gives the text to onData: a pasted line break submits the line, and
 *     the lines after it wait for the command before them (LineEditor.insertText);
 *   - select all selects the whole terminal, not the page;
 *   - Escape clears a selection; without one, it reaches the editor and interrupts the command that runs.
 * Without a selection, Ctrl+C does nothing. Returns a function that removes the listeners.
 */
export function attachShortcuts(terminal) {
    const element = terminal.element, textarea = terminal.textarea;
    let staged = false;  // the selection waits in the textarea for the engine's copy or cut
    const unstage = () => {
        if (!staged) return;
        staged = false;
        textarea.value = '';
    };
    terminal.attachCustomKeyEventHandler(event => {
        const action = shortcutOf(event, terminal.hasSelection());
        if (event.type !== 'keydown') return action === null;
        unstage();
        if (action === null) return true;
        if (action === 'selectAll' || action === 'clearSelection') {
            event.preventDefault();  // the page's own selection stays as it is
            if (action === 'selectAll') terminal.selectAll(); else terminal.clearSelection();
        } else if ((action === 'copy' || action === 'cut') && terminal.hasSelection()) {
            textarea.value = terminal.getSelection();
            textarea.select();
            staged = true;
        }
        return false;  // xterm.js leaves the event to the engine, which copies, cuts or pastes
    });
    // The engine enables Copy and Cut for a selection of the page; cancelling beforecopy and beforecut enables them for
    // the terminal's own selection too. xterm.js fills the clipboard on copy; a cut does the same, the terminal unchanged.
    const enable = event => { if (terminal.hasSelection()) event.preventDefault(); };
    const cut = event => {
        if (!terminal.hasSelection()) return;
        event.clipboardData?.setData('text/plain', terminal.getSelection());
        event.preventDefault();
    };
    const listeners = [['beforecopy', enable], ['beforecut', enable], ['cut', cut], ['copy', unstage], ['cut', unstage], ['blur', unstage]];
    for (const [type, listener] of listeners) (type === 'blur' ? textarea : element).addEventListener(type, listener);
    return () => {
        terminal.attachCustomKeyEventHandler(() => true);
        unstage();
        for (const [type, listener] of listeners) (type === 'blur' ? textarea : element).removeEventListener(type, listener);
    };
}

// --- Line editor -----------------------------------------------------------------------------------------------

// Editing keys, as xterm.js reports them in onData, mapped to LineEditor actions. Ctrl+C is not one of them: it copies
// (attachShortcuts), and the interruption of a command is Escape or the Stop button.
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
    ['\t', editor => editor.track(editor.completeThenReplay())],
    ['\x0c', editor => editor.clearScreen()],
    ['\x15', editor => editor.killBeforeCursor()],
    ['\x0b', editor => editor.killAfterCursor()],
]);

export const historyLimit = 500;

/**
 * Line editor driven by the terminal's input data.
 *   write(text): displays text; columns() and rows(): the terminal's size;
 *   run(line, signal): a Promise of the command's output; it rejects with an Error whose exitCode (1 by default, 130
 *     once interrupt() aborted `signal`) becomes the status;
 *   complete(line): a Promise of {candidates}, the full words that can replace the word at the end of the line (its
 *     value as wordAtEnd reads it);
 *   remember(line): persists a line added to the history.
 * Commands run one at a time; lines pasted together run one after the other.
 */
export class LineEditor {
    constructor({write, columns = () => 80, rows = () => 24, run, complete = async () => ({candidates: []}), prompt = '> ', history = [], remember = () => {}}) {
        Object.assign(this, {columns, rows, run, complete, persist: remember});
        // Whether the cursor is at the start of a row: output streamed without a final line break is followed by one
        // before the prompt.
        this.atLineStart = true;
        this.write = text => {
            if (!text) return;
            this.atLineStart = /\n$|\x1b\[H$/.test(text);
            write(text);
        };
        this.setPrompt(prompt);
        this.cells = [];
        this.cursor = 0;
        // The line's rows that fit the screen are drawn from row `first` of its layout; the cursor is `row` rows below
        // the first one drawn. Rows above the screen are in the scrollback, which cursor moves cannot reach.
        this.first = 0;
        this.row = 0;
        this.history = history.slice(-historyLimit);
        this.historyIndex = this.history.length;
        this.ahead = [];  // keys typed or pasted while a command runs, applied after it; '\r' ends a line
        this.status = 0;
        this.abort = null;
        this.pending = new Set();  // commands and completions in progress
        this.running = false;
        this.completing = null;  // input received while a completion is pending, replayed after it
        this.closed = false;
    }

    get line() { return this.text(0); }

    setPrompt(prompt) {
        this.prompt = prompt;
        ({clusters: this.promptCells, preceding: this.promptState} = cells(prompt));
    }

    /** A Promise settled once the commands and completions now in progress are; null when there is none. */
    get busy() {
        return this.pending.size ? Promise.allSettled([...this.pending]) : null;
    }

    /** Resolves once no command, typed-ahead line or completion is pending. */
    async whenIdle() {
        while (this.busy) await this.busy;
    }

    showPrompt() {
        this.row = this.first = 0;
        this.write(this.prompt);
        if (this.cells.length) this.redraw();
    }

    /**
     * Prints `text` as lines of their own, for an event that no command reported: above the line being edited, which is
     * drawn again below them (also while a completion is pending, which then acts on the line drawn again), or after the
     * output of the command that runs.
     */
    notice(text) {
        const lines = text.endsWith('\n') ? text : `${text}\r\n`;
        if (this.running || this.closed) return this.write((this.atLineStart ? '' : '\r\n') + lines);
        this.write(ansi.up(this.row) + '\r' + ansi.clearBelow + lines);
        this.showPrompt();
    }

    handle(data) {
        if (this.closed) return data === '\r' ? this.reopen() : undefined;
        if (this.completing) return void this.completing.push(data);
        if (this.running) return this.whileBusy(data);
        const action = keyActions.get(data);
        if (action) return action(this);
        if (!data.startsWith('\x1b')) return this.insertText(data);
    }

    // While a command runs, Escape (a lone ESC, which no other key sends) interrupts it and drops the keys typed after
    // it; other keys, typed or pasted, wait for the command and are applied after it.
    whileBusy(data) {
        if (data === interruptKey) return this.interrupt();
        this.typeAhead(data);
    }

    // Keeps input for later, each line break as its own '\r'.
    typeAhead(data) {
        data.split(/\r\n?|\n/).forEach((part, index) => {
            if (index > 0) this.ahead.push('\r');
            if (part) this.ahead.push(part);
        });
    }

    // Applies the keys typed ahead to the line, up to a line break: returns the line it submits, or null once the keys
    // are used up, leaving what they typed on the line. Tab, which would wait for a completion, is ignored there.
    readAhead() {
        while (this.ahead.length) {
            const data = this.ahead.shift();
            if (data === '\r') {
                this.moveTo(this.cells.length);
                const line = this.line;
                this.write('\r\n');
                this.cells = [];
                this.cursor = 0;
                this.row = this.first = 0;
                return line;
            }
            const action = keyActions.get(data);
            if (action && data !== '\t') action(this);
            else if (!action && !data.startsWith('\x1b')) this.insert(data);
        }
        return null;
    }

    /** Whether a command runs that interrupt() would stop: it has not been interrupted yet. */
    get interruptible() {
        return this.abort?.signal.aborted === false;
    }

    /**
     * Interrupts the command that runs (Escape, the Stop button): its signal is aborted, it ends with 130, and the keys
     * typed after it are dropped. Nothing happens while no command runs.
     */
    interrupt() {
        if (!this.running) return;
        this.ahead = [];
        if (!this.interruptible) return;
        this.write(`${this.atLineStart ? '' : '\r\n'}${ansi.dim('Interrupted')}\r\n`);
        this.abort.abort(new Error('Interrupted'));
    }

    // Typed or pasted text: printable characters are inserted; each line break submits the line, and the lines after
    // the first wait for the command before them.
    insertText(data) {
        const [first, ...rest] = data.split(/\r\n?|\n/);
        this.insert(first);
        if (!rest.length) return;
        this.typeAhead(rest.join('\n'));
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

    clearScreen() {
        this.write(ansi.clearScreen);
        this.showPrompt();
    }

    // The rows of the line to draw: all of them when they fit the screen, else a screen of rows around the cursor,
    // which stays where it was drawn while the cursor stays in it.
    window(cursorRow, lastRow) {
        const height = Math.max(1, this.rows());
        let first = this.first;
        if (lastRow < height) first = 0;
        else if (cursorRow < first) first = cursorRow;
        else if (cursorRow >= first + height) first = cursorRow - height + 1;
        first = Math.max(0, Math.min(first, lastRow - height + 1));
        return {first, last: Math.min(lastRow, first + height - 1)};
    }

    // Rewrites the line from its first drawn row, then places the cursor. A line taller than the screen is drawn a
    // screen at a time: its rows above the screen cannot be rewritten. A line that exactly fills its last row leaves
    // xterm.js's cursor waiting to wrap: a line break moves it where the layout expects it.
    redraw() {
        const columns = Math.max(1, this.columns());
        const all = [...this.promptCells, ...this.cells];
        const {starts, end} = layout(all, columns);
        const cursor = this.cursor === this.cells.length ? end : starts[this.promptCells.length + this.cursor];
        const {first, last} = this.window(cursor.row, end.row);
        let text = ansi.up(this.row) + '\r' + ansi.clearBelow;
        text += all.filter((cell, index) => starts[index].row >= first && starts[index].row <= last).map(cell => cell.text).join('');
        let written = last;
        if (end.row <= last) {
            written = end.row;
            if (end.column === 0 && end.row > first) text += '\r\n';
        }
        text += ansi.up(written - cursor.row) + '\r' + ansi.right(cursor.column);
        this.first = first;
        this.row = cursor.row - first;
        this.write(text);
    }

    /** After a resize, xterm.js reflows the rows: the cursor keeps its place in the line, on a recomputed row. */
    resized() {
        const columns = Math.max(1, this.columns());
        const {starts, end} = layout([...this.promptCells, ...this.cells], columns);
        const cursor = this.cursor === this.cells.length ? end : starts[this.promptCells.length + this.cursor];
        this.first = this.window(cursor.row, end.row).first;
        this.row = cursor.row - this.first;
    }

    track(promise) {
        const tracked = promise.finally(() => this.pending.delete(tracked));
        this.pending.add(tracked);
        return tracked;
    }

    remember(line) {
        if (line.trim() === '' || this.history.at(-1) === line) return;
        this.history.push(line);
        if (this.history.length > historyLimit) this.history.splice(0, this.history.length - historyLimit);
        try { this.persist(line); } catch { /* the history is a convenience */ }
    }

    // Moves below the line, runs it, then the lines typed or pasted ahead.
    submit() {
        this.moveTo(this.cells.length);
        const line = this.line;
        this.write('\r\n');
        this.cells = [];
        this.cursor = 0;
        this.row = this.first = 0;
        this.running = true;
        return this.track(this.runLines(line).finally(() => { this.running = false; }));
    }

    async runLines(line) {
        for (;;) {
            this.remember(line);
            this.historyIndex = this.history.length;
            await this.execute(line);
            if (this.closed) return;
            this.showPrompt();
            const next = this.readAhead();
            if (next === null) return;
            line = next;
        }
    }

    async execute(line) {
        if (line.trim() === '') return;
        this.abort = new AbortController();
        const {signal} = this.abort;
        try {
            const output = await this.run(line, signal);
            this.status = 0;
            this.write(output);
        } catch (error) {
            this.status = error?.exitCode ?? (signal.aborted ? 130 : 1);
            const message = String(error?.message ?? error);
            // interrupt() already said so: a bare interruption adds nothing. A code other than 1 is shown, unless the
            // message already gives it ("exited with code 3").
            const stated = this.status === 1 || new RegExp(`\\bcode ${this.status}\\b`).test(message);
            if (!(signal.aborted && message === 'Interrupted'))
                this.write(ansi.red(`Error: ${message}${stated ? '' : ` (exit code ${this.status})`}`).replace(/(?<!\r)\n/g, '\r\n') + '\r\n');
        } finally {
            this.abort = null;
            // The prompt starts on a new line, unless the output already ends one or leaves the cursor at home.
            if (!this.atLineStart) this.write('\r\n');
        }
    }

    // Keys typed while the completion is pending are kept and applied once it is inserted, in order: once one of them
    // submits the line, the next ones act on its command like keys typed while it runs (Escape interrupts it).
    async completeThenReplay() {
        this.completing = [];
        try { await this.completeLine(); }
        finally {
            const typed = this.completing;
            this.completing = null;
            for (const data of typed) this.handle(data);
        }
    }

    // Completes the word before the cursor, at the end of the line: one candidate is inserted whole, several by their
    // common prefix, or listed when they share nothing more.
    // The completion is escaped like the word it extends; a complete word also closes the quote it opened.
    async completeLine() {
        if (this.cursor !== this.cells.length) return;
        const line = this.line;
        const word = wordAtEnd(line);
        if (!word) return;
        let candidates;
        try { ({candidates} = await this.complete(line)); }
        catch { return; }
        if (!candidates?.length || this.line !== line) return;
        const extend = (text, end = '') => {
            const escaped = escapeFor(text.slice(word.value.length), word.quote);
            if (escaped !== null && (escaped || end)) this.insert(escaped + end);
        };
        if (candidates.length === 1) {
            const [candidate] = candidates;
            return extend(candidate, candidate.endsWith('/') ? '' : (word.quote ?? '') + ' ');
        }
        const prefix = commonPrefix(candidates);
        if (prefix.length > word.value.length) return extend(prefix);
        this.moveTo(this.cells.length);
        this.write('\r\n' + candidates.join('  ') + '\r\n');
        this.showPrompt();
    }

    /** Ends the session: the editor reads no more input until Enter starts a new session. */
    close() {
        this.closed = true;
        this.ahead = [];
    }

    reopen() {
        this.closed = false;
        this.status = 0;
        this.showPrompt();
    }
}
