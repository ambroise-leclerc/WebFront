// Specs of the terminal's keys (#721): the system's shortcuts reach the engine instead of becoming control characters,
// \x03 neither clears the line nor interrupts, and Escape or the Stop button interrupts a command with code 130. The
// engine's own copy and paste, which a script cannot trigger, are checked with real keys in test/web/developer-specs.mjs.
import {Terminal} from '@xterm/xterm';
import {LineEditor, shortcutOf, attachShortcuts, interruptKey} from '../../../js/developer/environment/terminal.mjs';

const keyCodes = {a: 65, c: 67, e: 69, v: 86, x: 88, Escape: 27};

/** A keydown as the keyboard sends it; xterm.js reads keyCode, which KeyboardEvent's constructor does not set. */
function keydown(target, {key, code = key.length === 1 ? `Key${key.toUpperCase()}` : key, ...modifiers}) {
    const event = new KeyboardEvent('keydown', {key, code, bubbles: true, cancelable: true, ...modifiers});
    Object.defineProperty(event, 'keyCode', {get: () => keyCodes[key] ?? 0});
    target.dispatchEvent(event);
    return event;
}

/** A clipboard event carrying its own DataTransfer, as the engine dispatches it for a copy, a cut or a paste. */
function clipboardEvent(target, type, text) {
    const clipboardData = new DataTransfer();
    if (text !== undefined) clipboardData.setData('text/plain', text);
    const event = new ClipboardEvent(type, {clipboardData, bubbles: true, cancelable: true});
    target.dispatchEvent(event);
    return event;
}

/** A command that runs until its signal is aborted, then ends like an interrupted command of the environment. */
const untilInterrupted = (line, signal) => new Promise((resolve, reject) => {
    if (line.startsWith('echo ')) return resolve(`${line.slice(5)}\r\n`);
    signal.addEventListener('abort', () => reject(Object.assign(new Error('Interrupted'), {exitCode: 130})), {once: true});
});

function editorOf({run = untilInterrupted} = {}) {
    const output = [];
    const signals = [];
    const editor = new LineEditor({write: text => output.push(text), run: (line, signal) => {
        signals.push(signal);
        return run(line, signal);
    }});
    return {editor, output, signals, text: () => output.join('')};
}

describe('The terminal', () => {
    describe('shortcuts', () => {
        it('are the system\'s copy, paste, cut and select all, with Ctrl or ⌘, and Escape on a selection', () => {
            const cases = [
                [{key: 'c', ctrlKey: true}, false, 'copy'],
                [{key: 'c', metaKey: true}, true, 'copy'],
                [{key: 'C', ctrlKey: true, shiftKey: true}, true, 'copy'],
                [{key: 'v', ctrlKey: true}, false, 'paste'],
                [{key: 'v', metaKey: true}, false, 'paste'],
                [{key: 'x', ctrlKey: true}, true, 'cut'],
                [{key: 'a', ctrlKey: true}, false, 'selectAll'],
                [{key: 'a', metaKey: true}, false, 'selectAll'],
                // The letter the layout gives: Ctrl+A on AZERTY is the key in the place of Q.
                [{key: 'a', code: 'KeyQ', ctrlKey: true}, false, 'selectAll'],
                [{key: 'q', code: 'KeyA', ctrlKey: true}, false, null],
                // Without Latin letters, the key's place, as the system does (Cyrillic с on the C key).
                [{key: 'с', code: 'KeyC', ctrlKey: true}, true, 'copy'],
                // AltGr sets Ctrl and Alt on Windows: a character, not a shortcut.
                [{key: 'c', ctrlKey: true, altKey: true}, true, null],
                [{key: 'c'}, true, null],
                [{key: 'e', ctrlKey: true}, false, null],
                [{key: 'Escape'}, true, 'clearSelection'],
                [{key: 'Escape'}, false, null],
                [{key: 'Escape', shiftKey: true}, true, null],
            ];
            for (const [event, selection, expected] of cases)
                expect(shortcutOf({code: '', ...event}, selection)).withContext(`${JSON.stringify(event)} selection=${selection}`).toBe(expected);
        });
    });

    describe('in xterm.js', () => {
        let host, terminal, data;
        const written = text => new Promise(resolve => terminal.write(text, resolve));
        beforeEach(async () => {
            host = document.createElement('div');
            host.style.cssText = 'width:640px;height:240px';
            document.body.append(host);
            terminal = new Terminal({cols: 80, rows: 10});
            terminal.open(host);
            data = [];
            terminal.onData(text => data.push(text));
            await written('hello copy\r\nsecond row\r\n');
        });
        afterEach(() => {
            terminal.dispose();
            host.remove();
        });

        it('sends \\x03 for Ctrl+C without the shortcuts: the keys of these specs reach its key handling', () => {
            keydown(terminal.textarea, {key: 'c', ctrlKey: true});
            expect(data).toEqual(['\x03']);
        });

        it('leaves Ctrl+C and ⌘C on a selection to the engine\'s copy, which puts the selection on the clipboard', () => {
            attachShortcuts(terminal);
            for (const modifier of ['ctrlKey', 'metaKey']) {
                terminal.select(0, 0, 5);
                const event = keydown(terminal.textarea, {key: 'c', [modifier]: true});
                expect(data).withContext(modifier).toEqual([]);
                expect(event.defaultPrevented).withContext('the engine copies').toBeFalse();
                // The selection waits in the focused textarea, selected, so that a native Edit menu enables Copy.
                const {value, selectionStart, selectionEnd} = terminal.textarea;
                expect([value, selectionStart, selectionEnd]).toEqual(['hello', 0, 5]);
                const copy = clipboardEvent(terminal.textarea, 'copy');
                expect(copy.clipboardData.getData('text/plain')).toBe('hello');
                expect(copy.defaultPrevented).toBeTrue();
                expect(terminal.textarea.value).withContext('the textarea is emptied after the copy').toBe('');
            }
            // The engine enables Copy and Cut for the terminal's selection.
            expect(clipboardEvent(terminal.element, 'beforecopy').defaultPrevented).toBeTrue();
            expect(clipboardEvent(terminal.element, 'beforecut').defaultPrevented).toBeTrue();
            terminal.clearSelection();
            expect(clipboardEvent(terminal.element, 'beforecopy').defaultPrevented).withContext('nothing to copy').toBeFalse();
        });

        it('copies on Ctrl+X, since nothing of the terminal can be cut', () => {
            attachShortcuts(terminal);
            terminal.select(0, 1, 6);
            keydown(terminal.textarea, {key: 'x', ctrlKey: true});
            const cut = clipboardEvent(terminal.textarea, 'cut');
            expect([data, cut.clipboardData.getData('text/plain'), cut.defaultPrevented, terminal.textarea.value]).toEqual([[], 'second', true, '']);
            expect(terminal.getSelection()).toBe('second');
        });

        it('selects the whole terminal on Ctrl+A and ⌘A, not the page', () => {
            attachShortcuts(terminal);
            for (const modifier of ['ctrlKey', 'metaKey']) {
                terminal.clearSelection();
                const event = keydown(terminal.textarea, {key: 'a', [modifier]: true});
                expect([data, event.defaultPrevented, terminal.getSelection().startsWith('hello copy')]).withContext(modifier).toEqual([[], true, true]);
            }
        });

        it('leaves Ctrl+V to the engine\'s paste, whose lines reach the editor as before', () => {
            attachShortcuts(terminal);
            const event = keydown(terminal.textarea, {key: 'v', ctrlKey: true});
            expect([data, event.defaultPrevented]).toEqual([[], false]);
            clipboardEvent(terminal.textarea, 'paste', 'echo one\necho two');
            expect(data).toEqual(['echo one\recho two']);
        });

        it('clears a selection on Escape, and sends Escape to the editor without one', () => {
            attachShortcuts(terminal);
            terminal.select(0, 0, 5);
            const event = keydown(terminal.textarea, {key: 'Escape'});
            expect([data, event.defaultPrevented, terminal.hasSelection()]).toEqual([[], true, false]);
            keydown(terminal.textarea, {key: 'Escape'});
            expect(data).toEqual([interruptKey]);
        });

        it('passes the other keys with Ctrl to the editor', () => {
            attachShortcuts(terminal);
            keydown(terminal.textarea, {key: 'e', ctrlKey: true});
            expect(data).toEqual(['\x05']);
        });

        it('lets Ctrl+C without a selection leave the line being typed as it is', async () => {
            attachShortcuts(terminal);
            const {editor} = editorOf();
            terminal.onData(text => editor.handle(text));
            terminal.input('echo kept');
            keydown(terminal.textarea, {key: 'c', ctrlKey: true});
            expect([editor.line, editor.busy, data]).toEqual(['echo kept', null, ['echo kept']]);
        });
    });

    describe('line editor', () => {
        it('neither clears the line nor interrupts a command on \\x03', async () => {
            const {editor, signals, text} = editorOf();
            editor.handle('echo kept');
            editor.handle('\x03');
            expect(editor.line).toBe('echo kept');
            editor.handle('\x15');
            editor.handle('sleep\r');
            editor.handle('\x03');
            expect(signals[0].aborted).withContext('\\x03 is not an interruption').toBeFalse();
            expect(text()).not.toContain('^C');
            editor.interrupt();
            await editor.whenIdle();
            expect(editor.status).toBe(130);
        });

        it('interrupts the command that runs on Escape, with code 130, and drops the keys typed after it', async () => {
            const {editor, signals, text} = editorOf();
            editor.handle('sleep\r');
            editor.handle('echo dropped\r');
            editor.handle(interruptKey);
            await editor.whenIdle();
            expect([editor.status, signals.length, signals[0].aborted, editor.line]).toEqual([130, 1, true, '']);
            expect(text().match(/Interrupted/g)).withContext('said once, without an error line').toEqual(['Interrupted']);
            expect(text()).not.toContain('dropped');
        });

        it('interrupts like the Stop button with interrupt(), once, and only while a command runs', async () => {
            const {editor, signals} = editorOf();
            editor.interrupt();
            editor.handle(interruptKey);  // idle: Escape does nothing
            expect([editor.line, editor.interruptible]).toEqual(['', false]);
            editor.handle('sleep\r');
            expect(editor.interruptible).toBeTrue();
            editor.interrupt();
            expect(editor.interruptible).withContext('an interrupted command is not interrupted again').toBeFalse();
            editor.interrupt();
            await editor.whenIdle();
            expect([editor.status, signals[0].aborted]).toEqual([130, true]);
            editor.handle('echo after\r');
            await editor.whenIdle();
            expect(editor.status).withContext('the next command runs').toBe(0);
        });

        it('interrupts the command that Enter submitted while a completion was pending', async () => {
            let answer;
            const editor = new LineEditor({write: () => {}, run: untilInterrupted,
                complete: () => new Promise(resolve => { answer = resolve; })});
            editor.handle('sle');
            editor.handle('\t');
            editor.handle('\r');
            editor.handle(interruptKey);
            answer({candidates: ['sleep']});
            await editor.whenIdle();
            expect(editor.status).toBe(130);
        });
    });
});
