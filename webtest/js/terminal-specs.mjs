// Drives the embedded terminal component (served by fs::TerminalFS) against the commands that
// webtest/JasmineTest.cpp registers through webfront::Terminal. Trailing spaces are trimmed when
// reading the screen, so a prompt line reads 'test>'.
import { mount } from '/webfront/components/terminal/terminal.mjs';

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
});
