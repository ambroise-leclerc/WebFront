import {test} from 'node:test';
import assert from 'node:assert/strict';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
import {resolvePackages, downloadPackage, validateLock} from '../environment/packages.bundle.mjs';
import {packageEntry, normalize, virtualPlugin} from '../environment/bundler.mjs';
import {generateProvider, compress} from '../environment/embed.mjs';
import {environmentFiles, bootstrap} from '../environment/bootstrap.mjs';
import {publish, responseBytes} from '../environment/io.mjs';
import {tar} from './archive.mjs';
import {parseSpec, verifyIntegrity, gunzip, readTar} from '../environment/tarball.mjs';

const encoder = new TextEncoder();
const encode = value => encoder.encode(typeof value === 'string' ? value : JSON.stringify(value));
const integrity = bytes => 'sha512-' + createHash('sha512').update(bytes).digest('base64');
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
        const manifest = {dist: {tarball: `https://registry.test/${path}.tgz`, integrity: integrity(pkg.bytes)}};
        return Response.json({'dist-tags': {latest: pkg.version}, versions: {[pkg.version]: manifest}});
    };
}

test('resolves transitive semver edges, cycles, multiple versions and required peers', async () => {
    const source = {
        app: {version: '1.0.0', bytes: fixture('app', '1.0.0', {dependencies: {dep: '^2.0.0'}, peerDependencies: {react: '^19.0.0'}})},
        dep: {version: '2.1.0', bytes: fixture('dep', '2.1.0', {dependencies: {app: '1.0.0'}})},
        react: {version: '19.0.0', bytes: fixture('react', '19.0.0')},
    };
    const options = {registry: 'https://registry.test/', fetchImpl: registry(source)};
    const {lock} = await resolvePackages(['app@latest', 'react@19.0.0'], options);
    assert.equal(lock.packages['app@1.0.0'].dependencies.dep, 'dep@2.1.0');
    assert.equal(lock.packages['dep@2.1.0'].dependencies.app, 'app@1.0.0');
    assert.equal(lock.packages['app@1.0.0'].dependencies.react, 'react@19.0.0');
    assert.equal(validateLock(lock), lock);
    await assert.rejects(resolvePackages(['app'], options), /requires peer react/);
    const download = async (name, range) => {
        const version = name === 'dep' ? (range.startsWith('^1') ? '1.2.0' : '2.0.0') : '1.0.0';
        const manifest = {name, version, dependencies: name === 'a' ? {dep: '^1.0.0'} : name === 'b' ? {dep: '^2.0.0'} : {}};
        return {id: idOf(name, version), key: name, manifest, integrity: 'sha512-test', files: new Map()};
    };
    const multiple = await resolvePackages(['a', 'b'], {download});
    assert.equal(multiple.lock.packages['a@1.0.0'].dependencies.dep, 'dep@1.2.0');
    assert.equal(multiple.lock.packages['b@1.0.0'].dependencies.dep, 'dep@2.0.0');
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

test('an installed lock pins transitive versions and rejects changed archives', async () => {
    const requests = [];
    const first = await resolvePackages(['app'], {download: async (name, range) => lockDownloads(requests)(name, range === '^2.0.0' ? '2.1.0' : range)});
    assert.equal(first.lock.packages['app@1.0.0'].dependencies.dep, 'dep@2.1.0');
    requests.length = 0;
    const again = await resolvePackages(['app@1.0.0'], {download: lockDownloads(requests), previous: first.lock});
    assert.equal(again.lock.packages['app@1.0.0'].dependencies.dep, 'dep@2.1.0');
    assert.ok(requests.includes('dep@2.1.0') && !requests.includes('dep@^2.0.0'), requests.join());
    await assert.rejects(resolvePackages(['app@1.0.0'], {download: lockDownloads(requests, true), previous: first.lock}),
        /dep@2\.1\.0 no longer matches the integrity recorded in the lock/);
    const fresh = await resolvePackages(['app@1.0.0'], {download: lockDownloads(requests)});
    assert.equal(fresh.lock.packages['app@1.0.0'].dependencies.dep, 'dep@2.2.0');
});

test('rejects integrity, archive identity, traversal and installation scripts', async () => {
    for (const [bytes, pattern] of [
        [fixture('wrong', '1.0.0'), /identity mismatch/],
        [fixture('pkg', '1.0.0', {scripts: {postinstall: 'node setup.js'}}), /unsupported install script/],
        [fixture('pkg', '1.0.0', {}, {'package/../escape': encode('bad')}), /Unsafe package path/],
        [fixture('pkg', '1.0.0', {}, {'package/addon.node': encode('bad')}), /native module/],
    ]) {
        await assert.rejects(downloadPackage('pkg', 'latest', {registry: 'https://registry.test/', fetchImpl: registry({pkg: {bytes, version: '1.0.0'}})}), pattern);
    }
    const bytes = fixture('pkg', '1.0.0');
    const fetchImpl = async url => String(url).endsWith('.tgz') ? new Response(bytes) : Response.json({versions: {'1.0.0': {dist: {tarball: 'https://registry.test/pkg.tgz', integrity: integrity(encode('other'))}}}});
    await assert.rejects(downloadPackage('pkg', '1.0.0', {registry: 'https://registry.test/', fetchImpl}), /Integrity check failed/);
});

test('resolves browser conditional exports and rejects private and Node imports', async () => {
    assert.equal(packageEntry({exports: {'.': {browser: './browser.js', default: './node.js'}, './*': './src/*.mjs'}}, ''), 'browser.js');
    assert.equal(packageEntry({exports: {'./*': './src/*.mjs'}}, 'sub'), 'src/sub.mjs');
    assert.throws(() => packageEntry({exports: {'.': './index.js'}}, 'private'), /does not export/);
    assert.equal(packageEntry({exports: {browser: {import: './b.mjs'}, default: './c.js'}}, '', 'require'), 'c.js');
    assert.throws(() => normalize('../outside'), /escapes/);
    assert.equal(packageEntry({exports: {'.': {import: './esm.js', require: './cjs.js'}}}, '', 'require'), 'cjs.js');
    let resolve;
    virtualPlugin({packages: {}, roots: {}}, {}, async () => undefined).setup({onResolve: (_, cb) => { resolve = cb; }, onLoad() {}});
    await assert.rejects(resolve({path: 'node:fs', importer: 'entry.tsx', namespace: 'project', kind: 'import-statement'}), /Unsupported browser import/);
});

test('generated gzip words decode to the exact resources and generation is reproducible', () => {
    const files = new Map([['index.html', encode('<div>PI</div>')], ['app.js', encode('console.log("π")')]]);
    const header = generateProvider(files, 'PiUI', 'MIT */ preserved');
    assert.equal(header, generateProvider(new Map([...files].reverse()), 'PiUI', 'MIT */ preserved'));
    const blocks = [...header.matchAll(/dataSize\{(\d+)\};\s+static constexpr[\s\S]*?data\{([\s\S]*?)\n        \};/g)];
    assert.equal(blocks.length, 2);
    for (const [index, match] of blocks.entries()) {
        const hex = [...match[2].matchAll(/0x([a-f0-9]{16})ULL/g)].map(word => word[1]).join('');
        const compressed = Buffer.from(hex, 'hex').subarray(0, Number(match[1]));
        assert.deepEqual(gunzipSync(compressed), Buffer.from([...files].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)[index][1]));
        assert.deepEqual([...compressed.subarray(4, 8)], [0, 0, 0, 0]);
    }
    assert.throws(() => generateProvider(files, '../bad'), /provider name/);
});

test('environment integrity and protocol are checked before publication', async () => {
    const archive = compress(tar(new Map([['package/environment.json', encode({protocol: 1, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('export function mount() {}')]])));
    const environment = await environmentFiles(archive, integrity(archive));
    assert.equal(environment.manifest.entry, 'runtime.mjs');
    await assert.rejects(environmentFiles(archive, integrity(encode('wrong'))), /Integrity check failed/);
    const wrong = compress(tar(new Map([['package/environment.json', encode({protocol: 2, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('')]])));
    await assert.rejects(environmentFiles(wrong, integrity(wrong)), /Incompatible WebFront developer environment: protocol version 2, this application supports 1/);
    // The API an environment expects must be one the C++ declares; an environment from before "api" reads as API 1.
    const withApi = api => compress(tar(new Map([['package/environment.json', encode({protocol: 1, api, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('')]])));
    const newer = withApi(2);
    await assert.rejects(environmentFiles(newer, integrity(newer)), /api version 2, this application supports 1/);
    assert.equal((await environmentFiles(newer, integrity(newer), {protocol: [1], api: [1, 2]})).manifest.api, 2);
    await assert.rejects(environmentFiles(archive, integrity(archive), {protocol: [1], api: [2]}), /api version 1, this application supports 2/);
    const current = withApi(1);
    assert.equal((await environmentFiles(current, integrity(current))).manifest.api, 1);
    const missing = compress(tar(new Map([['package/runtime.mjs', encode('')]])));
    await assert.rejects(environmentFiles(missing, integrity(missing)), /no environment\.json/);
});

test('aborted publications never commit and clean their staging transaction', async () => {
    const calls = [];
    const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return 'transaction'; }};
    const controller = new AbortController();
    controller.abort(new Error('Interrupted'));
    await assert.rejects(publish(bridge, 'generated.hpp', new Map([['output', encode('header')]]), {directory: false, signal: controller.signal}), /Interrupted/);
    assert.equal(calls.some(([name]) => name.endsWith('.commit')), false);
    assert.equal(calls.at(-1)[0], 'webfront.developer.abort');
});

test('checked-in browser bundles match their source and pinned dependency inputs', async () => {
    const manifest = JSON.parse(await readFile(new URL('../environment/bundles.json', import.meta.url)));
    for (const [path, hash] of Object.entries(manifest)) {
        const actual = createHash('sha256').update(await readFile(new URL('../../../' + path, import.meta.url))).digest('hex');
        assert.equal(actual, hash, `${path} changed: run js/developer/tools/build.mjs --refresh-bootstrap`);
    }
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
            assert.equal(previous.length, offset);
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

test('a pinned cached environment bootstraps offline and failed refreshes preserve publications', async () => {
    globalThis.location = new URL('http://localhost:9002/webfront-bootstrap.html');
    const archive = compress(tar(new Map([['package/environment.json', encode({protocol: 1, entry: 'runtime.mjs'})], ['package/runtime.mjs', encode('export function mount() {}')]])));
    const storage = new Map([['.webfront/developer/environment.tgz', archive]]);
    const bridge = memoryBridge(storage);
    const config = {environment: 'https://offline.test/environment.tgz', integrity: integrity(archive)};
    const offline = async () => { throw new Error('Offline'); };
    const result = await bootstrap(bridge, config, offline);
    assert.equal(result.url, '/webfront/developer/environment/runtime.mjs');
    assert.ok(storage.has('.webfront/developer/environment/runtime.mjs'));
    const before = storage.get('.webfront/developer/environment/runtime.mjs');
    await assert.rejects(bootstrap(bridge, {...config, integrity: integrity(encode('different'))}, offline), /Offline/);
    assert.equal(storage.get('.webfront/developer/environment/runtime.mjs'), before);
    delete globalThis.location;
});

test('compressed downloads stop at their byte limit', async () => {
    await assert.rejects(responseBytes(new Response(new Uint8Array(9)), 8), /byte limit/);
});

test('npm tarballs: package specs, integrity, bounded gunzip and tar entries', async () => {
    assert.deepEqual(parseSpec('plot'), {name: 'plot', version: ''});
    assert.deepEqual(parseSpec('plot@1.2.0'), {name: 'plot', version: '1.2.0'});
    assert.deepEqual(parseSpec('@acme/plot'), {name: '@acme/plot', version: ''});
    assert.deepEqual(parseSpec('@acme/plot@next'), {name: '@acme/plot', version: 'next'});
    const files = new Map([['package/package.json', encode({name: 'plot'})], ['package/dist/index.js', encode('x')]]);
    const archive = compress(tar(files));
    assert.equal(await verifyIntegrity(archive, `sha1-ignored ${integrity(archive)}`), integrity(archive));
    await assert.rejects(verifyIntegrity(archive, integrity(encode('other'))), /Integrity check failed/);
    await assert.rejects(verifyIntegrity(archive, 'sha1-only'), /no SHA-512 integrity/);
    const unpacked = await gunzip(archive);
    assert.deepEqual(readTar(unpacked).map(entry => entry.path).sort(), [...files.keys()].sort());
    await assert.rejects(gunzip(archive, unpacked.length - 1), /unpacks to more than/);
    assert.deepEqual(readTar(new Uint8Array(1024)), []);
    const corrupt = unpacked.slice();
    corrupt[148] ^= 1;
    assert.throws(() => readTar(corrupt), /Corrupt tar header/);
});

test('the terminal and webfront::Commands reserve the same command names', async () => {
    const {reservedNames} = await import('../environment/commands.mjs');
    const header = await readFile(new URL('../../../include/webfront/Commands.hpp', import.meta.url), 'utf8');
    const declared = header.match(/reservedNames\{([^}]*)\}/);
    assert.ok(declared, 'Commands.hpp must declare commands::reservedNames');
    assert.deepEqual([...declared[1].matchAll(/"([^"]*)"/g)].map(match => match[1]).sort(), [...reservedNames].sort());
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

// A bridge linked to the fake WebSocket, little-endian, and a helper answering a C++ call with a void return.
async function linkedBridge() {
    const source = await readFile(new URL('../../core/WebFront.js', import.meta.url), 'utf8');
    const sandbox = {WebSocket: FakeWebSocket, location: {host: 'localhost'}, TextEncoder, TextDecoder, console};
    vm.runInNewContext(source, sandbox);
    const bridge = sandbox.webFront, socket = FakeWebSocket.last;
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

test('a C++ command whose start was acknowledged rejects when the connection closes, and unsubscribes', async () => {
    const {runCommand} = await import('../environment/commands.mjs');
    const {bridge, socket, acknowledge} = await linkedBridge();
    const listenersBefore = socket.listeners;
    const command = runCommand(bridge, 'wait', []);
    command.catch(() => {});
    const start = socket.sent.at(-1);
    acknowledge(start);
    assert.equal(await settled(command), 'pending');
    assert.equal(socket.listeners, listenersBefore + 1);
    socket.fire('close', {wasClean: false});
    await assert.rejects(command, error => error.exitCode === 1 && /connection to the application closed/.test(error.message));
    assert.equal(socket.listeners, listenersBefore);
    // The exit report of the stopped command can no longer reach it; a late one is ignored.
    globalThis.webfront.commands.exit(commandNumber(start), 130, '');
    await assert.rejects(runCommand(bridge, 'wait', []), /not connected/);
    assert.equal(socket.listeners, listenersBefore);
});

test('a C++ command that exits, or is interrupted, removes its close listener', async () => {
    const {runCommand} = await import('../environment/commands.mjs');
    const {bridge, socket, acknowledge} = await linkedBridge();
    const listenersBefore = socket.listeners;
    const finished = runCommand(bridge, 'echo', ['x']);
    const start = socket.sent.at(-1);
    acknowledge(start);
    globalThis.webfront.commands.exit(commandNumber(start), 0, '');
    assert.equal(await finished, '');
    assert.equal(socket.listeners, listenersBefore);
    const controller = new AbortController();
    const interrupted = runCommand(bridge, 'wait', [], {signal: controller.signal});
    acknowledge(socket.sent.at(-1));
    controller.abort();
    await assert.rejects(interrupted, error => error.exitCode === 130);
    assert.equal(socket.listeners, listenersBefore);
    socket.fire('close', {wasClean: true, code: 1000, reason: ''});
});
