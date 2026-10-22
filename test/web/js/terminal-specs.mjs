// Drives the embedded terminal component (served by fs::TerminalFS) against the commands that
// test/web/JasmineTest.cpp registers through webfront::Terminal. Trailing spaces are trimmed when
// reading the screen, so a prompt line reads 'test>'.
import { mount, runLine, tokenize } from '/webfront/components/terminal/terminal.mjs';

describe('WebFront terminal component', () => {
    let mounted;
    let element;

    beforeAll(async () => {
        element = document.createElement('div');
        element.style.width = '640px';
        element.style.height = '240px';
        document.body.appendChild(element);
        mounted = await mount(element);
    });

    afterAll(() => {
        mounted.terminal.dispose();
        element.remove();
    });

    it('loads the compressed xterm CSS and Fit addon locally and resizes the terminal', async () => {
        const initialColumns = mounted.terminal.cols;
        expect(initialColumns).toBeGreaterThan(20);
        element.style.width = '960px';
        try {
            const deadline = Date.now() + 3000;
            while (mounted.terminal.cols <= initialColumns && Date.now() < deadline)
                await new Promise(resolve => setTimeout(resolve, 20));
            expect(mounted.terminal.cols).toBeGreaterThan(initialColumns);
            const style = getComputedStyle(element.querySelector('.xterm'));
            expect(style.position).toBe('relative');
            const resources = performance.getEntriesByType('resource')
                .filter(entry => entry.name.includes('/webfront/components/terminal/'));
            for (const name of ['xterm.mjs', 'xterm.css', 'addon-fit.mjs'])
                expect(resources.some(entry => entry.name.endsWith('/' + name))).toBeTrue();
            expect(resources.every(entry => new URL(entry.name).origin === location.origin)).toBeTrue();
        } finally {
            element.style.width = '640px';
            const deadline = Date.now() + 3000;
            while (mounted.terminal.cols !== initialColumns && Date.now() < deadline)
                await new Promise(resolve => setTimeout(resolve, 20));
        }
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

    it('splits lines into words like C++', () => {
        expect(tokenize(`greet 'Ada  L' "a\\"b" c\\ d`)).toEqual(['greet', 'Ada  L', 'a"b', 'c d']);
        expect(() => tokenize('"open')).toThrowError(SyntaxError, 'Unterminated quote');
        expect(() => tokenize('end\\')).toThrowError(SyntaxError, 'Trailing backslash');
    });

    it('runs browser commands of extensions before C++, which gets the lines they decline', async () => {
        const runInCpp = async (line) => `cpp:${line}`;
        const progress = [];
        const commands = new Map([['tool', async ([action], context) => {
            if (action !== 'local')
                return undefined;
            context.write('step\n');
            return `browser:${context.line}\nsecond line`;
        }]]);
        const options = {commands, runInCpp, write: (text) => progress.push(text)};
        expect(await runLine('tool local', options)).toBe('browser:tool local\r\nsecond line');
        expect(progress).toEqual(['step\r\n']);
        expect(await runLine('tool other', options)).toBe('cpp:tool other');
        expect(await runLine('greet', options)).toBe('cpp:greet');
        expect(await runLine('tool "open', options)).toBe('cpp:tool "open');
    });

    it('reports an extension module that fails to load and keeps the terminal usable', async () => {
        const element = document.createElement('div');
        element.style.width = '1000px';
        element.style.height = '120px';
        document.body.appendChild(element);
        const bridge = {
            ready: Promise.resolve(),
            cppFunction: (name) => async (line) => ({
                'webfront.terminal.prompt': '> ',
                'webfront.terminal.banner': '',
                'webfront.terminal.extensions': '/webfront/components/tools/missing.mjs',
                'webfront.terminal.run': `ran ${line}`,
            })[name],
        };
        const withExtension = await mount(element, { bridge });
        await withExtension.flush();
        const text = withExtension.terminal.buffer.active.getLine(0).translateToString(true);
        expect(text).toContain('Terminal extension /webfront/components/tools/missing.mjs failed to load');
        expect(withExtension.editor).not.toBeNull();
        withExtension.terminal.dispose();
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
});
