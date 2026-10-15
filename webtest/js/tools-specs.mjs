// Drives the wf tool manager: the browser module served by fs::ToolManagerFS, loaded by the terminal
// as an extension, against the webfront::ToolManager that webtest/JasmineTest.cpp registers with the
// fake npm registry under webtest/registry/ (no public network). Installed tools go to a temporary
// project directory, served by fs::BasicPackageFS.
import { mount } from '/webfront/components/terminal/terminal.mjs';
import { gunzip, installed, parseSpec, readTar, verifyIntegrity } from '/webfront/components/tools/wf.mjs';

const toolName = 'webfront-test-tool';
const toolUrl = `/webfront/tools/${toolName}/`;
const longPath = 'dist/assets/' + 'nested-directory-with-a-long-name/'.repeat(3) + 'file.txt';

async function fetchBytes(url) {
    const response = await fetch(url);
    if (!response.ok)
        throw new Error(`${url}: HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
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
        const output = await run('wf search test');
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
        const data = await fetchBytes(`${toolUrl}dist/data.bin`);  // written in three chunks
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
});
