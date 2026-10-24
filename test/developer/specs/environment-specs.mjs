// Specs of the environment's packages, bundler, bootstrap and C++ commands, run by `wf test` in the browser.
import {rejects, throws, integrity} from '../support.mjs';
import {resolvePackages, downloadPackage, validateLock} from '../../../js/developer/environment/packages.bundle.mjs';
import {packageEntry, normalize, virtualPlugin} from '../../../js/developer/environment/bundler.mjs';
import {generateProvider, compress} from '../../../js/developer/environment/embed.mjs';
import {environmentFiles, bootstrap} from '../../../js/developer/environment/bootstrap.mjs';
import {publish, responseBytes} from '../../../js/developer/environment/io.mjs';
import {tar} from '../../../js/developer/tools/archive.mjs';
import {parseSpec, verifyIntegrity, gunzip, readTar} from '../../../js/developer/environment/tarball.mjs';
import {runCommand} from '../../../js/developer/environment/commands.mjs';

describe('The environment', () => {
    const encoder = new TextEncoder();
    const encode = value => encoder.encode(typeof value === 'string' ? value : JSON.stringify(value));
    const idOf = (name, version) => `${name.replace('/', '+')}@${version}`;
    function fixture(name, version, fields = {}, extra = {}) {
        const files = new Map(Object.entries({'package/package.json': encode({name, version, ...fields}), ...extra}));
        return compress(tar(files));
    }
    function registry(packages) {
        return async url => {
            const path = new URL(url).pathname.slice(1);
            if (path.endsWith('.tgz')) return new Response(packages[path.slice(0, -4)].bytes);
            const pkg = packages[path];
            if (!pkg) return new Response('', {status: 404});
            const manifest = {dist: {tarball: `https://registry.test/${path}.tgz`, integrity: await integrity(pkg.bytes)}};
            return Response.json({'dist-tags': {latest: pkg.version}, versions: {[pkg.version]: manifest}});
        };
    }

    it('resolves transitive semver edges, cycles, multiple versions and required peers', async () => {
        const source = {
            app: {version: '1.0.0', bytes: fixture('app', '1.0.0', {dependencies: {dep: '^2.0.0'}, peerDependencies: {react: '^19.0.0'}})},
            dep: {version: '2.1.0', bytes: fixture('dep', '2.1.0', {dependencies: {app: '1.0.0'}})},
            react: {version: '19.0.0', bytes: fixture('react', '19.0.0')},
        };
        const options = {registry: 'https://registry.test/', fetchImpl: registry(source)};
        const {lock} = await resolvePackages(['app@latest', 'react@19.0.0'], options);
        expect(lock.packages['app@1.0.0'].dependencies.dep).toBe('dep@2.1.0');
        expect(lock.packages['dep@2.1.0'].dependencies.app).toBe('app@1.0.0');
        expect(lock.packages['app@1.0.0'].dependencies.react).toBe('react@19.0.0');
        expect(validateLock(lock)).toBe(lock);
        await rejects(resolvePackages(['app'], options), /requires peer react/);
        const download = async (name, range) => {
            const version = name === 'dep' ? (range.startsWith('^1') ? '1.2.0' : '2.0.0') : '1.0.0';
            const manifest = {name, version, dependencies: name === 'a' ? {dep: '^1.0.0'} : name === 'b' ? {dep: '^2.0.0'} : {}};
            return {id: idOf(name, version), key: name, manifest, integrity: 'sha512-test', files: new Map()};
        };
        const multiple = await resolvePackages(['a', 'b'], {download});
        expect(multiple.lock.packages['a@1.0.0'].dependencies.dep).toBe('dep@1.2.0');
        expect(multiple.lock.packages['b@1.0.0'].dependencies.dep).toBe('dep@2.0.0');
    });

    // Fake registry: dep 2.1.0 is installed, 2.2.0 is the newest; `tampered` changes the archive of 2.1.0.
    const depIntegrity = {'2.1.0': 'sha512-first', '2.2.0': 'sha512-second'};
    function lockDownloads(requests, tampered = false) {
        return async (name, range) => {
            requests.push(`${name}@${range}`);
            const version = name === 'dep' ? (depIntegrity[range] ? range : '2.2.0') : '1.0.0';
            const manifest = {name, version, dependencies: name === 'app' ? {dep: '^2.0.0'} : {}};
            let integrity = 'sha512-app';
            if (name === 'dep') integrity = tampered && version === '2.1.0' ? 'sha512-other' : depIntegrity[version];
            return {id: idOf(name, version), key: `${name}-${version}`, manifest, integrity, files: new Map()};
        };
    }

    it('an installed lock pins transitive versions and rejects changed archives', async () => {
        const requests = [];
        const first = await resolvePackages(['app'], {download: async (name, range) => lockDownloads(requests)(name, range === '^2.0.0' ? '2.1.0' : range)});
        expect(first.lock.packages['app@1.0.0'].dependencies.dep).toBe('dep@2.1.0');
        requests.length = 0;
        const again = await resolvePackages(['app@1.0.0'], {download: lockDownloads(requests), previous: first.lock});
        expect(again.lock.packages['app@1.0.0'].dependencies.dep).toBe('dep@2.1.0');
        expect(requests.includes('dep@2.1.0') && !requests.includes('dep@^2.0.0')).withContext(requests.join()).toBeTruthy();
        await rejects(resolvePackages(['app@1.0.0'], {download: lockDownloads(requests, true), previous: first.lock}),
            /dep@2\.1\.0 no longer matches the integrity recorded in the lock/);
        const fresh = await resolvePackages(['app@1.0.0'], {download: lockDownloads(requests)});
        expect(fresh.lock.packages['app@1.0.0'].dependencies.dep).toBe('dep@2.2.0');
    });

    it('rejects integrity, archive identity, traversal and installation scripts', async () => {
        for (const [bytes, pattern] of [
            [fixture('wrong', '1.0.0'), /identity mismatch/],
            [fixture('pkg', '1.0.0', {scripts: {postinstall: 'node setup.js'}}), /unsupported install script/],
            [fixture('pkg', '1.0.0', {}, {'package/../escape': encode('bad')}), /Unsafe package path/],
            [fixture('pkg', '1.0.0', {}, {'package/addon.node': encode('bad')}), /native module/],
        ]) {
            await rejects(downloadPackage('pkg', 'latest', {registry: 'https://registry.test/', fetchImpl: registry({pkg: {bytes, version: '1.0.0'}})}), pattern);
        }
        const bytes = fixture('pkg', '1.0.0');
        const fetchImpl = async url => String(url).endsWith('.tgz') ? new Response(bytes) : Response.json({versions: {'1.0.0': {dist: {tarball: 'https://registry.test/pkg.tgz', integrity: await integrity(encode('other'))}}}});
        await rejects(downloadPackage('pkg', '1.0.0', {registry: 'https://registry.test/', fetchImpl}), /Integrity check failed/);
    });

    it('resolves browser conditional exports and rejects private and Node imports', async () => {
        expect(packageEntry({exports: {'.': {browser: './browser.js', default: './node.js'}, './*': './src/*.mjs'}}, '')).toBe('browser.js');
        expect(packageEntry({exports: {'./*': './src/*.mjs'}}, 'sub')).toBe('src/sub.mjs');
        throws(() => packageEntry({exports: {'.': './index.js'}}, 'private'), /does not export/);
        expect(packageEntry({exports: {browser: {import: './b.mjs'}, default: './c.js'}}, '', 'require')).toBe('c.js');
        throws(() => normalize('../outside'), /escapes/);
        expect(packageEntry({exports: {'.': {import: './esm.js', require: './cjs.js'}}}, '', 'require')).toBe('cjs.js');
        let resolve;
        virtualPlugin({packages: {}, roots: {}}, {}, async () => undefined).setup({onResolve: (_, cb) => { resolve = cb; }, onLoad() {}});
        await rejects(resolve({path: 'node:fs', importer: 'entry.tsx', namespace: 'project', kind: 'import-statement'}), /Unsupported browser import/);
    });

    it('generated gzip words decode to the exact resources and generation is reproducible', async () => {
        const files = new Map([['index.html', encode('<div>PI</div>')], ['app.js', encode('console.log("π")')]]);
        const header = generateProvider(files, 'PiUI', 'MIT */ preserved');
        expect(header).toBe(generateProvider(new Map([...files].reverse()), 'PiUI', 'MIT */ preserved'));
        const blocks = [...header.matchAll(/dataSize\{(\d+)\};\s+static constexpr[\s\S]*?data\{([\s\S]*?)\n        \};/g)];
        expect(blocks.length).toBe(2);
        for (const [index, match] of blocks.entries()) {
            const hex = [...match[2].matchAll(/0x([a-f0-9]{16})ULL/g)].map(word => word[1]).join('');
            const compressed = Uint8Array.from(hex.match(/../g), byte => parseInt(byte, 16)).subarray(0, Number(match[1]));
            expect(await gunzip(compressed)).toEqual([...files].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)[index][1]);
            expect([...compressed.subarray(4, 8)]).toEqual([0, 0, 0, 0]);
        }
        throws(() => generateProvider(files, '../bad'), /provider name/);
    });

    it('environment integrity and protocol are checked before publication', async () => {
        const archive = compress(tar(new Map([['package/environment.json', encode({protocol: 1, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('export function mount() {}')]])));
        const environment = await environmentFiles(archive, await integrity(archive));
        expect(environment.manifest.entry).toBe('runtime.mjs');
        await rejects(environmentFiles(archive, await integrity(encode('wrong'))), /Integrity check failed/);
        const wrong = compress(tar(new Map([['package/environment.json', encode({protocol: 2, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('')]])));
        await rejects(environmentFiles(wrong, await integrity(wrong)), /Incompatible WebFront developer environment: protocol version 2, this application supports 1/);
        // The API an environment expects must be one the C++ declares; an environment from before "api" reads as API 1.
        const withApi = api => compress(tar(new Map([['package/environment.json', encode({protocol: 1, api, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('')]])));
        const newer = withApi(2);
        await rejects(environmentFiles(newer, await integrity(newer)), /api version 2, this application supports 1/);
        expect((await environmentFiles(newer, await integrity(newer), {protocol: [1], api: [1, 2]})).manifest.api).toBe(2);
        await rejects(environmentFiles(archive, await integrity(archive), {protocol: [1], api: [2]}), /api version 1, this application supports 2/);
        const current = withApi(1);
        expect((await environmentFiles(current, await integrity(current))).manifest.api).toBe(1);
        const missing = compress(tar(new Map([['package/runtime.mjs', encode('')]])));
        await rejects(environmentFiles(missing, await integrity(missing)), /no environment\.json/);
    });

    it('aborted publications never commit and clean their staging transaction', async () => {
        const calls = [];
        const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return 'transaction'; }};
        const controller = new AbortController();
        controller.abort(new Error('Interrupted'));
        await rejects(publish(bridge, 'generated.hpp', new Map([['output', encode('header')]]), {directory: false, signal: controller.signal}), /Interrupted/);
        expect(calls.some(([name]) => name.endsWith('.commit'))).toBe(false);
        expect(calls.at(-1)[0]).toBe('webfront.developer.abort');
    });

    // An in-memory project behind the webfront.developer.* bridge callbacks.
    function memoryBridge(storage) {
        const transactions = new Map();
        let next = 0;
        const operations = {
            read: ([path, offset]) => {
                const bytes = storage.get(path);
                if (!bytes) throw new Error('Cannot read project file: ' + path);
                return bytes.subarray(offset, offset + 256 * 1024);
            },
            begin: ([destination, directory]) => {
                const id = String(++next);
                transactions.set(id, {destination, directory, files: new Map()});
                return id;
            },
            write: ([id, path, offset, bytes]) => {
                const files = transactions.get(id).files;
                const previous = files.get(path) ?? new Uint8Array();
                expect(previous.length).toBe(offset);
                files.set(path, new Uint8Array([...previous, ...bytes]));
            },
            commit: ([id]) => {
                const {destination, directory, files} = transactions.get(id);
                for (const [path, bytes] of files) storage.set(directory ? `${destination}/${path}` : destination, bytes);
                transactions.delete(id);
            },
            abort: ([id]) => { transactions.delete(id); },
        };
        return {cppFunction: name => async (...args) => operations[name.split('.').pop()](args)};
    }

    it('a pinned cached environment bootstraps offline and failed refreshes preserve publications', async () => {
        const archive = compress(tar(new Map([['package/environment.json', encode({protocol: 1, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('export function mount() {}')]])));
        const storage = new Map([['.webfront/developer/environment.tgz', archive]]);
        const bridge = memoryBridge(storage);
        const config = {environment: 'https://offline.test/environment.tgz', integrity: await integrity(archive)};
        const offline = async () => { throw new Error('Offline'); };
        const result = await bootstrap(bridge, config, offline);
        expect(result.url).toBe('/webfront/developer/environment/runtime.mjs');
        expect(storage.has('.webfront/developer/environment/runtime.mjs')).toBeTruthy();
        const before = storage.get('.webfront/developer/environment/runtime.mjs');
        await rejects(bootstrap(bridge, {...config, integrity: await integrity(encode('different'))}, offline), /Offline/);
        expect(storage.get('.webfront/developer/environment/runtime.mjs')).toBe(before);
    });

    it('compressed downloads stop at their byte limit', async () => {
        await rejects(responseBytes(new Response(new Uint8Array(9)), 8), /byte limit/);
    });

    it('npm tarballs: package specs, integrity, bounded gunzip and tar entries', async () => {
        expect(parseSpec('plot')).toEqual({name: 'plot', version: ''});
        expect(parseSpec('plot@1.2.0')).toEqual({name: 'plot', version: '1.2.0'});
        expect(parseSpec('@acme/plot')).toEqual({name: '@acme/plot', version: ''});
        expect(parseSpec('@acme/plot@next')).toEqual({name: '@acme/plot', version: 'next'});
        const files = new Map([['package/package.json', encode({name: 'plot'})], ['package/dist/index.js', encode('x')]]);
        const archive = compress(tar(files));
        expect(await verifyIntegrity(archive, `sha1-ignored ${await integrity(archive)}`)).toBe(await integrity(archive));
        await rejects(verifyIntegrity(archive, await integrity(encode('other'))), /Integrity check failed/);
        await rejects(verifyIntegrity(archive, 'sha1-only'), /no SHA-512 integrity/);
        const unpacked = await gunzip(archive);
        expect(readTar(unpacked).map(entry => entry.path).sort()).toEqual([...files.keys()].sort());
        await rejects(gunzip(archive, unpacked.length - 1), /unpacks to more than/);
        expect(readTar(new Uint8Array(1024))).toEqual([]);
        const corrupt = unpacked.slice();
        corrupt[148] ^= 1;
        throws(() => readTar(corrupt), /Corrupt tar header/);
    });

    // A WebSocket the real bridge (js/core/WebFront.js) talks to: it records the frames sent, fires the on* handlers like a
    // browser, and counts the listeners added with addEventListener.
    class FakeWebSocket extends EventTarget {
        static last = null;
        constructor() {
            super();
            this.sent = [];
            this.listeners = 0;
            FakeWebSocket.last = this;
        }
        send(data) { this.sent.push(new Uint8Array(data.buffer ?? data).slice()); }
        addEventListener(...args) { ++this.listeners; super.addEventListener(...args); }
        removeEventListener(...args) { --this.listeners; super.removeEventListener(...args); }
        fire(type, init = {}) {
            const event = Object.assign(new Event(type), init);
            this[`on${type}`]?.(event);
            this.dispatchEvent(event);
        }
    }

    // A bridge linked to the fake WebSocket, little-endian, and a helper answering a C++ call with a void return. The real
    // bridge (js/core/WebFront.js, as the application serves it) runs in a frame of its own, so that it neither replaces nor
    // is replaced by the one of the page that runs the specs.
    const frames = [];
    afterEach(() => { for (const frame of frames.splice(0)) frame.remove(); });
    async function linkedBridge() {
        const source = await (await fetch('/WebFront.js')).text();  // the embedded bridge: the runner page loads the same file
        const frame = document.body.appendChild(document.createElement('iframe'));
        frames.push(frame);
        frame.hidden = true;
        frame.contentWindow.WebSocket = FakeWebSocket;
        frame.contentWindow.eval(source);
        const bridge = frame.contentWindow.webFront, socket = FakeWebSocket.last;
        socket.fire('open');
        socket.fire('message', {data: new Uint8Array([1, 0]).buffer});  // acknowledgement: little-endian
        await bridge.ready;
        const acknowledge = frame => socket.fire('message', {data: new Uint8Array([4, 0, frame[2], frame[3], 0, 0, 0, 0]).buffer});
        return {bridge, socket, acknowledge};
    }

    // The command number of a webfront.commands.start frame: the number after the function name.
    const commandNumber = frame => {
        const view = new DataView(frame.buffer);
        return view.getFloat64(8 + 2 + view.getUint8(9) + 1, true);
    };

    const settled = promise => Promise.race([promise.then(() => 'resolved', () => 'rejected'), new Promise(resolve => setTimeout(() => resolve('pending'), 20))]);

    it('a C++ command whose start was acknowledged rejects when the connection closes, and unsubscribes', async () => {
        const {bridge, socket, acknowledge} = await linkedBridge();
        const listenersBefore = socket.listeners;
        const command = runCommand(bridge, 'wait', []);
        command.catch(() => {});
        const start = socket.sent.at(-1);
        acknowledge(start);
        expect(await settled(command)).toBe('pending');
        expect(socket.listeners).toBe(listenersBefore + 1);
        socket.fire('close', {wasClean: false});
        await rejects(command, error => error.exitCode === 1 && /connection to the application closed/.test(error.message));
        expect(socket.listeners).toBe(listenersBefore);
        // The exit report of the stopped command can no longer reach it; a late one is ignored.
        globalThis.webfront.commands.exit(commandNumber(start), 130, '');
        await rejects(runCommand(bridge, 'wait', []), /not connected/);
        expect(socket.listeners).toBe(listenersBefore);
    });

    it('a C++ command that exits, or is interrupted, removes its close listener', async () => {
        const {bridge, socket, acknowledge} = await linkedBridge();
        const listenersBefore = socket.listeners;
        const finished = runCommand(bridge, 'echo', ['x']);
        const start = socket.sent.at(-1);
        acknowledge(start);
        globalThis.webfront.commands.exit(commandNumber(start), 0, '');
        expect(await finished).toBe('');
        expect(socket.listeners).toBe(listenersBefore);
        const controller = new AbortController();
        const interrupted = runCommand(bridge, 'wait', [], {signal: controller.signal});
        acknowledge(socket.sent.at(-1));
        controller.abort();
        await rejects(interrupted, error => error.exitCode === 130);
        expect(socket.listeners).toBe(listenersBefore);
        socket.fire('close', {wasClean: true, code: 1000, reason: ''});
    });

});
