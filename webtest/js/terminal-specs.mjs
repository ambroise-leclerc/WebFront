// Drives the embedded terminal component (served by fs::TerminalFS) against the commands that
// webtest/JasmineTest.cpp registers through webfront::Terminal. Trailing spaces are trimmed when
// reading the screen, so a prompt line reads 'test>'.
import { mount } from '/webfront/components/terminal/terminal.mjs';
import { hooks } from './browser-command-fixture.mjs';

describe('WebFront terminal component', () => {
    let mounted;

    beforeAll(async () => {
        const element = document.createElement('div');
        element.style.width = '640px';
        element.style.height = '240px';
        document.body.appendChild(element);
        mounted = await mount(element);
    });

    function screen() {
        const buffer = mounted.terminal.buffer.active;
        const lines = [];
        for (let index = 0; index < buffer.length; ++index)
            lines.push(buffer.getLine(index).translateToString(true));
        return lines.join('\n').trimEnd();
    }

    // Each chunk is delivered like one keyboard event, then the command or completion is awaited.
    async function type(...chunks) {
        for (const chunk of chunks) {
            mounted.terminal.input(chunk);
            await mounted.editor.whenIdle();
        }
        await mounted.flush();
        return screen();
    }

    it('shows the banner and prompt read from C++', async () => {
        await mounted.flush();
        expect(mounted.editor).not.toBeNull();
        expect(screen()).toContain('webtest terminal');
        expect(screen().split('\n').at(-1)).toBe('test>');
    });

    it('runs a registered C++ command with its parsed arguments', async () => {
        const text = await type('greet "Ada Lovelace"\r');
        expect(text).toContain('test> greet "Ada Lovelace"\nHello, Ada Lovelace!\ntest>');
    });

    it('reports an unknown command without failing the session', async () => {
        const text = await type('nope\r');
        expect(text).toContain("Unknown command 'nope'");
        expect(text.split('\n').at(-1)).toBe('test>');
    });

    it('shows a C++ command exception as an error', async () => {
        const text = await type('fail\r');
        expect(text).toContain('Error: terminal command failed');
        expect(text.split('\n').at(-1)).toBe('test>');
    });

    it('reports an application without terminal functions instead of failing to mount', async () => {
        const element = document.createElement('div');
        document.body.appendChild(element);
        const missing = {
            ready: Promise.resolve(),
            cppFunction: (name) => () => Promise.reject(new Error(`C++ function '${name}' was not found`)),
        };
        const failed = await mount(element, { bridge: missing });
        await failed.flush();
        expect(failed.editor).toBeNull();
        expect(failed.terminal.buffer.active.getLine(0).translateToString(true))
            .toContain("Terminal unavailable: C++ function 'webfront.terminal.");
        failed.terminal.dispose();
        element.remove();
    });

    // Specs run in random order, so this one does not depend on the history left by the others.
    it('completes a command name with Tab and recalls history', async () => {
        await type('gre', '\t');
        expect(mounted.editor.line).toBe('greet ');
        await type('Bob\r');
        expect(screen()).toContain('Hello, Bob!');
        await type('\x1b[A');
        expect(mounted.editor.line).toBe('greet Bob');
        await type('\x03');
        expect(mounted.editor.line).toBe('');
    });
    it('continues a command in the browser module that C++ names, with its parsed arguments', async () => {
        const text = await type('browser echo a "b c" \'\'\r');
        expect(text).toContain('from C++\nargs: ["a","b c",""], bridge: true\ntest>');
    });

    it('starts the prompt on a new line after browser output without a line end', async () => {
        const text = await type('browser partial\r');
        expect(text).toContain('from C++\nno line end\ntest>');
    });

    it('shows a browser command exception as an error on its own line', async () => {
        const text = await type('browser fail\r');
        expect(text).toContain('from C++\nstarted\nError: browser command failed\ntest>');
    });

    it('reports a module without run()', async () => {
        const text = await type('nomodule\r');
        expect(text).toContain('Error: /js/module-fixture.mjs does not export run(args, io)\ntest>');
    });

    it('aborts a running browser command with Ctrl+C and keeps the prompt locked until it ends', async () => {
        const waiting = new Promise((resolve) => { hooks.onWait = resolve; });
        mounted.terminal.input('browser wait\r');
        await waiting;
        mounted.terminal.input('ignored');
        expect(mounted.editor.line).toBe('');
        mounted.terminal.input('\x03');
        await mounted.editor.whenIdle();
        await mounted.flush();
        hooks.onWait = null;
        expect(screen()).toContain('from C++\nwaiting\n^C\nError: Interrupted\ntest>');
    });
});
