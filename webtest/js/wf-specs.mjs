// Drives the wf tool manager (include/components/ToolManager.hpp and its browser module) through
// the terminal component, against the local registry that webtest/MakeToolRegistry.cmake builds and
// webtest/JasmineTest.cpp serves under /registry/. Trailing spaces are trimmed when reading the screen.
import { mount } from '/webfront/components/terminal/terminal.mjs';
import { verifyIntegrity } from '/webfront/components/tools/wf.mjs';

const installedHello = "Installed webfront-hello@1.0.0: Hello tool. Type 'wf open webfront-hello' to open it.";

async function text(url) {
    const response = await fetch(url);
    expect(response.status).withContext(url).toBe(200);
    return response.text();
}

async function sha512(bytes) {
    return 'sha512-' + btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.digest('SHA-512', bytes))));
}

// A tar archive of [{path, text, type}] entries; a path over 100 bytes gets a GNU long-name entry.
function tar(entries) {
    const encoder = new TextEncoder();
    const blocks = [];
    const header = (name, size, type) => {
        const block = new Uint8Array(512);
        const put = (value, offset) => block.set(encoder.encode(value), offset);
        put(name.slice(0, 100), 0);
        put('0000644\0', 100);
        put('0000000\0', 108);
        put('0000000\0', 116);
        put(size.toString(8).padStart(11, '0') + '\0', 124);
        put('00000000000\0', 136);
        block.fill(0x20, 148, 156);
        put(type, 156);
        put('ustar\x0000', 257);
        put(block.reduce((sum, byte) => sum + byte, 0).toString(8).padStart(6, '0') + '\0 ', 148);
        return block;
    };
    const data = (bytes) => {
        const padded = new Uint8Array(Math.ceil(bytes.length / 512) * 512);
        padded.set(bytes);
        return padded;
    };
    for (const {path, text: content, type = '0'} of entries) {
        const bytes = encoder.encode(content);
        if (path.length > 100) {
            const name = encoder.encode(path + '\0');
            blocks.push(header('././@LongLink', name.length, 'L'), data(name));
        }
        blocks.push(header(path, bytes.length, type), data(bytes));
    }
    blocks.push(new Uint8Array(1024));
    return new Uint8Array(blocks.flatMap((block) => [...block]));
}

async function gzip(bytes) {
    return new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(new CompressionStream('gzip'))).arrayBuffer());
}

describe('WebFront wf tool manager', () => {
    let mounted;

    function screen() {
        const buffer = mounted.terminal.buffer.active;
        const lines = [];
        for (let index = 0; index < buffer.length; ++index)
            lines.push(buffer.getLine(index).translateToString(true));
        return lines.join('\n').trimEnd();
    }

    // Runs one line and returns what it printed, between its own line and the next prompt.
    async function run(line) {
        mounted.terminal.clear();
        mounted.terminal.input(line + '\r');
        await mounted.editor.whenIdle();
        await mounted.flush();
        const lines = screen().split('\n');
        const start = lines.findIndex((text) => text === `test> ${line}`);
        return lines.slice(start + 1, -1).join('\n');
    }

    beforeAll(async () => {
        const element = document.createElement('div');
        element.style.width = '1600px';
        element.style.height = '320px';
        document.body.appendChild(element);
        mounted = await mount(element, {terminalOptions: {scrollback: 0}});
        expect(await run('wf install webfront-hello')).toBe(installedHello);
    });

    it('installs a tool and serves every file of its tarball', async () => {
        expect(await run('wf list')).toMatch(/^webfront-hello@1\.0\.0 +Hello tool$/m);
        expect(await text('/webfront/tools/webfront-hello/dist/index.html')).toContain('Hello from an installed tool');
        expect(await text('/webfront/tools/webfront-hello/package.json')).toContain('"webfront-hello"');
        expect(await text('/webfront/tools/webfront-hello/dist/big.txt')).toBe('0123456789'.repeat(30000));
        expect(await text('/webfront/tools/webfront-hello/empty.txt')).toBe('');
        expect(await text(`/webfront/tools/webfront-hello/${'n'.repeat(120)}.txt`)).toBe('long name');
        expect(await text(`/webfront/tools/webfront-hello/deep/${'d'.repeat(60)}/${'f'.repeat(60)}.txt`)).toBe('long path');
    });

    it('reinstalls an installed tool', async () => {
        expect(await run('wf install webfront-hello@latest')).toBe(installedHello);
    });

    it('installs a scoped tool at a requested version, with the default entry page', async () => {
        expect(await run('wf install @webfront-test/plot@1.0.0'))
            .toBe("Installed @webfront-test/plot@1.0.0: Plot. Type 'wf open @webfront-test/plot' to open it.");
        expect(await text('/webfront/tools/@webfront-test/plot/index.html')).toContain('<p>Plot</p>');
    });

    it('searches the registry for tools', async () => {
        const output = await run('wf search hello');
        expect(output).toMatch(/^webfront-hello@1\.0\.0 +Says hello$/m);
        expect(output).toMatch(/^@webfront-test\/plot@1\.0\.0 +Plots values$/m);
    });

    it('opens an installed tool in a new window, or shows its URL when the window is blocked', async () => {
        const url = `${location.origin}/webfront/tools/webfront-hello/dist/index.html`;
        const open = spyOn(window, 'open').and.returnValue({});
        expect(await run('wf open webfront-hello')).toBe(`Opened ${url}`);
        expect(open).toHaveBeenCalledWith(url, '_blank');
        open.and.returnValue(null);
        expect(await run('wf open webfront-hello')).toBe(`The browser blocked the new window. Open ${url}`);
    });

    it('reports a missing package or version, a wrong hash and a package that is not a tool', async () => {
        expect(await run('wf install webfront-missing')).toMatch(/^Error: Package webfront-missing not found/);
        expect(await run('wf install webfront-hello@2.0.0')).toBe("Error: Version '2.0.0' of webfront-hello not found");
        expect(await run('wf install webfront-badhash')).toMatch(/^Downloading webfront-badhash@1\.0\.0…\nError: Integrity check failed/);
        expect(await run('wf install webfront-notool')).toMatch(/^Error: webfront-notool@1\.0\.0 is not a WebFront tool/);
        const list = await run('wf list');
        expect(list).not.toContain('webfront-badhash');
        expect(list).not.toContain('webfront-notool');
    });

    describe('with a registry stub', () => {
        const realFetch = window.fetch;
        let routes;

        beforeEach(() => {
            routes = new Map();
            spyOn(window, 'fetch').and.callFake((url, init) => {
                const route = routes.get(new URL(url, location.href).pathname);
                return route ? route(init) : realFetch(url, init);
            });
        });

        async function publish(name, entries) {
            const tarball = await gzip(tar(entries));
            const packument = {
                name,
                'dist-tags': {latest: '1.0.0'},
                versions: {'1.0.0': {name, version: '1.0.0', webfront: {kind: 'tool'},
                    dist: {tarball: `/registry/tarballs/${name}.tgz`, integrity: await sha512(tarball)}}},
            };
            routes.set(`/registry/${name}`, async () => new Response(JSON.stringify(packument)));
            routes.set(`/registry/tarballs/${name}.tgz`, async () => new Response(tarball));
        }

        it('lets C++ reject a tarball path escaping the tool, keeping the installed tools', async () => {
            await publish('webfront-evil', [
                {path: 'package/package.json', text: JSON.stringify({name: 'webfront-evil', version: '1.0.0', webfront: {kind: 'tool'}})},
                {path: 'package/index.html', text: '<p>evil</p>'},
                {path: `package/${'x'.repeat(100)}/../../evil.txt`, text: 'escaped'},
            ]);
            expect(await run('wf install webfront-evil')).toContain("Error: Unsafe path 'x");
            expect(await run('wf list')).not.toContain('webfront-evil');
            expect(await text('/webfront/tools/webfront-hello/dist/index.html')).toContain('Hello from an installed tool');
        });

        it('rejects a tarball whose package.json names another package', async () => {
            await publish('webfront-other', [
                {path: 'package/package.json', text: JSON.stringify({name: 'webfront-hello', version: '1.0.0', webfront: {kind: 'tool'}})},
                {path: 'package/index.html', text: '<p>other</p>'},
            ]);
            expect(await run('wf install webfront-other'))
                .toContain('Error: The tarball contains webfront-hello@1.0.0, not webfront-other@1.0.0');
        });

        it('stops a download with Ctrl+C', async () => {
            let started;
            const waiting = new Promise((resolve) => { started = resolve; });
            routes.set('/registry/webfront-slow', ({signal}) => new Promise((resolve, reject) => {
                signal.addEventListener('abort', () => reject(signal.reason));
                started();
            }));
            mounted.terminal.clear();
            mounted.terminal.input('wf install webfront-slow\r');
            await waiting;
            mounted.terminal.input('\x03');
            await mounted.editor.whenIdle();
            await mounted.flush();
            expect(screen()).toContain('test> wf install webfront-slow\n^C\nError: Interrupted\ntest>');
        });
    });

    it('checks the strongest hash of an integrity string', async () => {
        const bytes = new TextEncoder().encode('tool');
        const strongest = await sha512(bytes);
        await expectAsync(verifyIntegrity(bytes, `sha256-AAAA ${strongest}`)).toBeResolvedTo(strongest);
        await expectAsync(verifyIntegrity(bytes, 'sha512-AAAA sha256-AAAA')).toBeRejectedWithError(/Integrity check failed/);
        await expectAsync(verifyIntegrity(bytes, 'sha1-AAAA')).toBeRejectedWithError(/no SHA-256, SHA-384 or SHA-512/);
    });
});
