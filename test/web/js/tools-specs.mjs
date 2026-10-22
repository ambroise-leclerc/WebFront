// Drives the wf tool manager: the browser module served by fs::ToolManagerFS, loaded by the terminal
// as an extension, against the webfront::ToolManager that test/web/JasmineTest.cpp registers with the
// fake npm registry under test/web/registry/ (no public network). Installed tools go to a temporary
// project directory, served by fs::BasicPackageFS.
import { mount } from '/webfront/components/terminal/terminal.mjs';
import { chunkSize, gunzip, installed, parseSpec, readTar, verifyIntegrity } from '/webfront/components/tools/wf.mjs';

const toolName = 'webfront-test-tool';
const toolUrl = `/webfront/tools/${toolName}/`;
const longPath = 'dist/assets/' + 'nested-directory-with-a-long-name/'.repeat(3) + 'file.txt';

async function fetchBytes(url) {
    const response = await fetch(url);
    if (!response.ok)
        throw new Error(`${url}: HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
}

async function sha512(bytes) {
    return 'sha512-' + btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.digest('SHA-512', bytes))));
}

// A tar archive of [{path, text}] regular files; a path over 100 bytes gets a GNU long-name entry.
function tar(entries) {
    const encoder = new TextEncoder();
    const blocks = [];
    const padded = (bytes) => {
        const block = new Uint8Array(Math.ceil(bytes.length / 512) * 512);
        block.set(bytes);
        return block;
    };
    const header = (name, size, type) => {
        const block = new Uint8Array(512);
        const put = (value, offset) => block.set(encoder.encode(value), offset);
        put(name.slice(0, 100), 0);
        put('0000644\0', 100);
        put(size.toString(8).padStart(11, '0') + '\0', 124);
        put('00000000000\0', 136);
        block.fill(0x20, 148, 156);
        put(type, 156);
        put('ustar\x0000', 257);
        put(block.reduce((sum, byte) => sum + byte, 0).toString(8).padStart(6, '0') + '\0 ', 148);
        return block;
    };
    for (const {path, text} of entries) {
        const bytes = encoder.encode(text);
        if (path.length > 100) {
            const name = encoder.encode(path + '\0');
            blocks.push(header('././@LongLink', name.length, 'L'), padded(name));
        }
        blocks.push(header(path, bytes.length, '0'), padded(bytes));
    }
    blocks.push(new Uint8Array(1024));
    return new Uint8Array(blocks.flatMap((block) => [...block]));
}

async function gzip(bytes) {
    return new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(new CompressionStream('gzip'))).arrayBuffer());
}

async function fixtureTarball() {
    const packument = await (await fetch(`/registry/${toolName}`)).json();
    const {tarball, integrity} = packument.versions['1.0.0'].dist;
    return {bytes: await fetchBytes(`/registry/${tarball}`), integrity};
}

describe('WebFront wf tool manager', () => {
    let mounted;

    beforeAll(async () => {
        const element = document.createElement('div');
        element.style.width = '1200px';
        element.style.height = '320px';
        document.body.appendChild(element);
        mounted = await mount(element);
    });

    afterAll(() => {
        mounted.terminal.dispose();
    });

    // Wrapped rows are joined, so long output lines read as one.
    function screen() {
        const buffer = mounted.terminal.buffer.active;
        let text = '';
        for (let index = 0; index < buffer.length; ++index) {
            const line = buffer.getLine(index);
            text += (index > 0 && !line.isWrapped ? '\n' : '') + line.translateToString(true);
        }
        return text.trimEnd();
    }

    // Clears the screen, runs a line and returns what it printed, without the prompts.
    async function run(line) {
        for (const chunk of ['clear\r', line + '\r']) {
            mounted.terminal.input(chunk);
            await mounted.editor.whenIdle();
        }
        await mounted.flush();
        const lines = screen().split('\n');
        expect(lines[0]).toBe(`test> ${line}`);
        expect(lines.at(-1)).toBe('test>');
        return lines.slice(1, -1).join('\n');
    }

    async function isInstalled(name) {
        return (await installed()).some((tool) => tool.name === name);
    }

    it('splits package specs into a name and a version', () => {
        expect(parseSpec('plot')).toEqual({name: 'plot', version: ''});
        expect(parseSpec('plot@1.2.0')).toEqual({name: 'plot', version: '1.2.0'});
        expect(parseSpec('@acme/plot')).toEqual({name: '@acme/plot', version: ''});
        expect(parseSpec('@acme/plot@next')).toEqual({name: '@acme/plot', version: 'next'});
    });

    it('verifies the SHA-512 integrity of a tarball and reads its ustar, prefixed and pax paths', async () => {
        const {bytes, integrity} = await fixtureTarball();
        expect(await verifyIntegrity(bytes, `sha1-ignored ${integrity}`)).toBe(integrity);
        await expectAsync(verifyIntegrity(bytes, 'sha512-' + btoa('x'.repeat(64)))).toBeRejectedWithError(/Integrity check failed/);
        await expectAsync(verifyIntegrity(bytes, 'sha1-only')).toBeRejectedWithError(/no SHA-512 integrity/);

        const files = readTar(await gunzip(bytes));
        expect(files.map((file) => file.path)).toEqual([
            'package/package.json',
            'package/dist/index.html',
            'package/dist/data.bin',
            `package/${longPath}`,
            'package/dist/café.txt',
        ]);
        expect(files[2].bytes.length).toBe(40000);
    });

    it('rejects a corrupt tar header', async () => {
        const archive = readTar(new Uint8Array(1024));  // only end-of-archive blocks
        expect(archive).toEqual([]);
        const corrupt = new Uint8Array(1024);
        corrupt.set(new TextEncoder().encode('name'), 0);
        expect(() => readTar(corrupt)).toThrowError(/Corrupt tar header/);
    });

    it('lets C++ answer wf help', async () => {
        const output = await run('wf help');
        expect(output).toContain('wf install <package>[@version]');
        expect(output).toContain('wf remove <package>');
    });

    it('searches the registry for web tools', async () => {
        const fetched = spyOn(window, 'fetch').and.callThrough();
        const output = await run('wf search test');
        const url = new URL(fetched.calls.mostRecent().args[0], location.href);
        expect(url.pathname).toBe('/registry/-/v1/search');
        expect(url.searchParams.get('text')).toBe('keywords:webfront-tool test');
        expect(output).toContain(`${toolName}@1.0.0  Fixture of the WebFront browser integration test`);
    });

    it('installs a tool that is then listed, served, opened and removed', async () => {
        const installOutput = await run(`wf install ${toolName}`);
        expect(installOutput).toContain(`Downloading ${toolName}@1.0.0`);
        expect(installOutput).toContain('Installing 5 files');
        expect(installOutput).toContain(`Installed ${toolName}@1.0.0: Test tool.`);
        expect(await run('wf list')).toBe(`${toolName}@1.0.0  Test tool`);

        const page = new TextDecoder().decode(await fetchBytes(`${toolUrl}dist/index.html`));
        expect(page).toContain('webfront-test-tool page');
        const data = await fetchBytes(`${toolUrl}dist/data.bin`);
        expect(data.length).toBe(40000);
        expect(data.every((byte, index) => byte === index % 251)).toBeTrue();
        expect(new TextDecoder().decode(await fetchBytes(toolUrl + longPath))).toBe('long path\n');
        expect(new TextDecoder().decode(await fetchBytes(`${toolUrl}dist/${encodeURIComponent('café.txt')}`))).toBe('pax path\n');

        const open = spyOn(window, 'open').and.returnValue({});
        expect(await run(`wf open ${toolName}`)).toBe(`Opened Test tool: ${toolUrl}dist/index.html`);
        expect(open).toHaveBeenCalledOnceWith(`${toolUrl}dist/index.html`, '_blank');

        expect(await run(`wf remove ${toolName}`)).toBe(`Removed ${toolName}.`);
        expect((await fetch(`${toolUrl}dist/index.html`)).status).toBe(404);
    });

    it('refuses a tarball whose integrity does not match', async () => {
        const output = await run('wf install webfront-test-corrupt');
        expect(output).toContain('Error: Integrity check failed');
        expect(await isInstalled('webfront-test-corrupt')).toBeFalse();
    });

    it('reports an unknown version or package', async () => {
        expect(await run(`wf install ${toolName}@9.9.9`)).toContain(`Error: ${toolName} has no version '9.9.9'`);
        expect(await run('wf install webfront-missing-tool')).toMatch(/Error: .*\/registry\/webfront-missing-tool: HTTP 404/);
        expect(await isInstalled(toolName)).toBeFalse();
    });

    it('reports a tool that is not installed or malformed arguments', async () => {
        expect(await run('wf open webfront-missing-tool')).toBe('Error: webfront-missing-tool is not installed');
        expect(await run('wf install')).toBe("Error: Wrong number of arguments. Type 'wf help'.");
    });

    it('refuses a tarball that unpacks to more than the limit', async () => {
        const bytes = await gzip(new Uint8Array(4096));
        expect((await gunzip(bytes, 4096)).length).toBe(4096);
        await expectAsync(gunzip(bytes, 4095)).toBeRejectedWithError(/unpacks to more than 4095 bytes/);
    });

    // Packages built in the page and served by a fetch stub under the fake registry's URL.
    describe('with packages published in the page', () => {
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
                versions: {'1.0.0': {name, version: '1.0.0', dist: {tarball: `/registry/tarballs/${name}.tgz`, integrity: await sha512(tarball)}}},
            };
            routes.set(`/registry/${name}`, async () => new Response(JSON.stringify(packument)));
            routes.set(`/registry/tarballs/${name}.tgz`, async () => new Response(tarball));
        }

        const manifest = (name) => ({path: 'package/package.json',
            text: JSON.stringify({name, version: '1.0.0', webfront: {kind: 'tool', entry: 'index.html', title: name}})});

        it('installs files larger than a bridge chunk, which the browser sends in several WebSocket frames', async () => {
            const big = '0123456789'.repeat(Math.ceil(1.2 * chunkSize / 10));
            await publish('webfront-test-big', [manifest('webfront-test-big'),
                {path: 'package/index.html', text: '<p>big</p>'},
                {path: 'package/big.txt', text: big},
                {path: 'package/empty.txt', text: ''},
                {path: `package/${'n'.repeat(120)}.txt`, text: 'long name'}]);
            expect(await run('wf install webfront-test-big')).toContain('Installed webfront-test-big@1.0.0');
            const tool = '/webfront/tools/webfront-test-big/';
            expect(new TextDecoder().decode(await fetchBytes(`${tool}big.txt`))).toBe(big);
            expect((await fetchBytes(`${tool}empty.txt`)).length).toBe(0);
            expect(new TextDecoder().decode(await fetchBytes(`${tool}${'n'.repeat(120)}.txt`))).toBe('long name');
            expect(await run('wf remove webfront-test-big')).toBe('Removed webfront-test-big.');
        });

        it('lets C++ reject a path escaping the tool', async () => {
            await publish('webfront-test-evil', [manifest('webfront-test-evil'),
                {path: 'package/index.html', text: '<p>evil</p>'},
                {path: `package/${'x'.repeat(100)}/../../evil.txt`, text: 'escaped'}]);
            expect(await run('wf install webfront-test-evil')).toContain("Error: Unsafe path 'x");
            expect(await isInstalled('webfront-test-evil')).toBeFalse();
        });

        it('stops an installation with Ctrl+C', async () => {
            let started;
            const waiting = new Promise((resolve) => { started = resolve; });
            routes.set('/registry/webfront-test-slow', ({signal}) => new Promise((resolve, reject) => {
                signal.addEventListener('abort', () => reject(signal.reason));
                started();
            }));
            mounted.terminal.input('clear\r');
            await mounted.editor.whenIdle();
            mounted.terminal.input('wf install webfront-test-slow\r');
            await waiting;
            mounted.terminal.input('\x03');
            await mounted.editor.whenIdle();
            await mounted.flush();
            expect(screen()).toBe('test> wf install webfront-test-slow\n^C\nError: Interrupted\ntest>');
            expect(await isInstalled('webfront-test-slow')).toBeFalse();
        });
    });
});
