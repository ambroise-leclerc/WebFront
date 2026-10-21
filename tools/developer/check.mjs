import {test} from 'node:test';
import assert from 'node:assert/strict';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';
import {readFile} from 'node:fs/promises';
import {resolvePackages, downloadPackage, validateLock} from '../../src/developer/packages.bundle.mjs';
import {packageEntry, normalize, virtualPlugin} from '../../src/developer/bundler.mjs';
import {generateProvider, compress} from '../../src/developer/embed.mjs';
import {environmentFiles, bootstrap} from '../../src/developer/bootstrap.mjs';
import {publish, responseBytes} from '../../src/developer/io.mjs';
import {tar} from './archive.mjs';

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
    await assert.rejects(environmentFiles(wrong, integrity(wrong)), /Unsupported WebFront/);
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
    const manifest = JSON.parse(await readFile(new URL('../../src/developer/bundles.json', import.meta.url)));
    for (const [path, hash] of Object.entries(manifest)) {
        const actual = createHash('sha256').update(await readFile(new URL('../../' + path, import.meta.url))).digest('hex');
        assert.equal(actual, hash, `${path} changed: run tools/developer/build.mjs --refresh-bootstrap`);
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
