// Node tests of the project manifest, its agreement with the lock and the joint publication of both.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {validateManifest, parseManifest, serializeManifest, manifestName, dependenciesDigest} from '../environment/manifest.mjs';
import {install, initProject, loadProject, syncNotice, resolveEntry, lockPath, manifestPath} from '../environment/packages.bundle.mjs';
import {compress} from '../environment/embed.mjs';
import {tar} from './archive.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const encode = value => encoder.encode(typeof value === 'string' ? value : JSON.stringify(value));
const integrity = bytes => 'sha512-' + createHash('sha512').update(bytes).digest('base64');
const minimal = {format: 1, name: 'app', version: '0.1.0'};

test('a valid manifest receives its defaults and keeps a stable serialization', () => {
    const manifest = validateManifest(minimal);
    assert.deepEqual(manifest.dependencies, {});
    assert.deepEqual(manifest.entries, {});
    const full = {...minimal, dependencies: {react: '^19.0.0', '@scope/lib': '>=1.2.0 <2', tool: 'latest'}, registry: 'https://registry.test/',
        scripts: {build: 'wf embed main'}, entries: {main: 'ui/main.tsx'},
        providers: {PiUI: {entry: 'main', output: 'generated/PiUI.hpp'}}, test: {specs: ['spec/a.mjs']},
        permissions: {files: ['ui'], publish: ['generated'], fetch: ['https://registry.npmjs.org']}};
    const text = serializeManifest(validateManifest(full));
    assert.deepEqual(JSON.parse(text), full);
    assert.equal(serializeManifest(parseManifest(encode(text))), text);
    assert.equal(manifestName('apps/My App'), 'my-app');
    assert.equal(manifestName(''), 'webfront-app');
});

test('invalid manifests are explicit errors, never silent fallbacks', () => {
    const cases = [
        [{...minimal, extra: 1}, /unknown key "extra"/],
        [{name: 'app', version: '1.0.0'}, /format must be 1/],
        [{...minimal, format: 2}, /format 2 is not supported/],
        [{...minimal, name: 'Bad Name'}, /name must be/],
        [{...minimal, version: 'one'}, /version must be/],
        [{...minimal, dependencies: {react: 3}}, /invalid version range/],
        [{...minimal, dependencies: {'../x': '1.0.0'}}, /invalid npm package name/],
        [{...minimal, dependencies: {react: 'file:../react'}}, /invalid version range/],
        [{...minimal, registry: 'http://insecure.test/'}, /registry must use https/],
        [{...minimal, entries: {main: '../outside.tsx'}}, /safe project-relative path/],
        [{...minimal, entries: {main: '/etc/passwd'}}, /safe project-relative path/],
        [{...minimal, providers: {'not valid': {entry: 'a.tsx', output: 'a.hpp'}}}, /invalid provider name/],
        [{...minimal, providers: {P: {entry: 'a.tsx', output: 'a.js'}}}, /\.hpp file/],
        [{...minimal, providers: {P: {entry: 'a.tsx', output: 'a.hpp', more: 1}}}, /unknown key "more"/],
        [{...minimal, test: {specs: ['../spec.mjs']}}, /safe project-relative path/],
        [{...minimal, test: {runner: 'x'}}, /unknown key "runner"/],
        [{...minimal, permissions: {shell: true}}, /unknown permission "shell"/],
        [{...minimal, permissions: {files: ['a/../../b']}}, /safe project-relative path/],
        [{...minimal, permissions: {fetch: ['http://plain.test']}}, /https origin/],
        [{...minimal, permissions: {fetch: ['https://registry.test/path']}}, /without a path/],
        [[], /must be an object/],
    ];
    for (const [value, message] of cases) assert.throws(() => validateManifest(value), message);
    assert.throws(() => parseManifest(encode('{')), /not valid JSON/);
});

test('the dependency digest ignores key order and changes with any range', async () => {
    const first = await dependenciesDigest({a: '^1.0.0', b: '2.0.0'});
    assert.equal(first, await dependenciesDigest({b: '2.0.0', a: '^1.0.0'}));
    assert.notEqual(first, await dependenciesDigest({a: '^1.0.0', b: '^2.0.0'}));
    assert.match(first, /^[0-9a-f]{64}$/);
});

test('entries are named in the manifest or given as paths', () => {
    const manifest = validateManifest({...minimal, entries: {main: 'ui/main.tsx', other: 'ui/other.tsx'}});
    assert.equal(resolveEntry(manifest, undefined, 'wf preview'), 'ui/main.tsx');
    assert.equal(resolveEntry(manifest, 'other', 'wf preview'), 'ui/other.tsx');
    assert.equal(resolveEntry(manifest, 'free/path.tsx', 'wf preview'), 'free/path.tsx');
    assert.throws(() => resolveEntry(validateManifest(minimal), undefined, 'wf preview'), /entries.main/);
});

// An in-memory project behind the webfront.developer.* callbacks, with the store's group publication and precondition.
function memoryBridge(storage = new Map()) {
    const transactions = new Map();
    let next = 0;
    const stateOf = paths => paths.map(path => storage.has(path) ? createHash('sha256').update(storage.get(path)).digest('hex') : '-').join(',');
    const operations = {
        read: (path, offset) => {
            if (!storage.has(path)) throw new Error('Cannot read project file: ' + path);
            return storage.get(path).subarray(offset, offset + 256 * 1024);
        },
        begin: (destination, directory) => { transactions.set(String(++next), {destination, directory, files: new Map(), replace: true}); return String(next); },
        beginNew: (destination, directory) => { transactions.set(String(++next), {destination, directory, files: new Map(), replace: false}); return String(next); },
        write: (id, path, offset, bytes) => {
            const files = transactions.get(id).files;
            const previous = files.get(path) ?? new Uint8Array();
            assert.equal(previous.length, offset);
            files.set(path, new Uint8Array([...previous, ...bytes]));
        },
        commit: id => {
            const {destination, directory, files, replace} = transactions.get(id);
            if (!replace && (storage.has(destination) || [...storage.keys()].some(key => key.startsWith(destination + '/'))))
                throw new Error('Starter destination already exists');
            for (const [path, bytes] of files) storage.set(directory ? `${destination}/${path}` : destination, bytes);
            transactions.delete(id);
        },
        abort: id => { transactions.delete(id); },
        state: paths => stateOf(paths.split('\n')),
        commitGroup: (ids, paths, expected) => {
            if (stateOf(paths.split('\n')) !== expected) throw new Error('The project changed since it was read; run the command again');
            for (const id of ids.split('\n')) {
                const {destination, files} = transactions.get(id);
                storage.set(destination, files.get('output'));
                transactions.delete(id);
            }
        },
    };
    const bridge = {cppFunction: name => async (...args) => operations[name.replace('webfront.developer.', '')](...args)};
    return {bridge, storage, pending: () => transactions.size};
}

const tagged = (packages, tags) => async url => {
    const response = await packages(url);
    const body = response.headers.get('content-type')?.includes('json') ? await response.json() : null;
    if (!body) return response;
    return Response.json({...body, 'dist-tags': {...body['dist-tags'], ...tags}});
};
const pkg = (name, version, fields = {}) => compress(tar(new Map([['package/package.json', encode({name, version, ...fields})]])));
function registry(packages) {
    const requests = [];
    const fetchImpl = async url => {
        requests.push(String(url));
        const path = new URL(url).pathname.slice(1);
        if (path.endsWith('.tgz')) {
            const [name, version] = path.slice(0, -4).split('@');
            const entry = packages[name];
            return new Response(entry.older?.[version] ?? entry.bytes);
        }
        const entry = packages[path];
        if (!entry) return new Response('', {status: 404});
        const versions = {[entry.version]: entry.bytes, ...entry.older};
        const described = Object.fromEntries(Object.entries(versions).map(([version, bytes]) =>
            [version, {dist: {tarball: `https://registry.test/${path}@${version}.tgz`, integrity: integrity(bytes)}}]));
        return Response.json({'dist-tags': {latest: entry.version}, versions: described});
    };
    return {fetchImpl, requests};
}
const source = () => ({
    a: {version: '1.2.0', bytes: pkg('a', '1.2.0')},
    b: {version: '2.0.0', bytes: pkg('b', '2.0.0')},
    c: {version: '3.1.0', bytes: pkg('c', '3.1.0')},
});
const options = fetchImpl => ({registry: 'https://registry.test/', fetchImpl});
const json = (storage, path) => JSON.parse(decoder.decode(storage.get(path)));

async function project(...files) {
    const memory = memoryBridge();
    await initProject(memory.bridge, [], {});
    for (const spec of files) await install(memory.bridge, [spec], options(registry(source()).fetchImpl));
    return memory;
}

test('wf init creates a minimal manifest and never replaces an existing one', async () => {
    const {bridge, storage} = memoryBridge();
    assert.match(await initProject(bridge, [], {}), /Created webfront.project.json/);
    assert.deepEqual(json(storage, manifestPath), {format: 1, name: 'webfront-app', version: '0.1.0', dependencies: {}});
    await assert.rejects(initProject(bridge, [], {}), /already exists/);
    await initProject(bridge, ['apps/Ui'], {});
    assert.equal(json(storage, 'apps/Ui/webfront.project.json').name, 'ui');
    await assert.rejects(initProject(bridge, ['../escape'], {}), /Unsafe package path/);
    await assert.rejects(initProject(bridge, ['a', 'b'], {}), /Usage/);
});

test('wf install needs a manifest, then publishes manifest and lock together', async () => {
    const {bridge, storage} = memoryBridge();
    const {fetchImpl} = registry(source());
    await assert.rejects(install(bridge, ['a'], options(fetchImpl)), /run wf init/);
    await initProject(bridge, [], {});
    assert.equal((await loadProject(bridge)).status, 'unlocked');
    await install(bridge, ['a'], options(fetchImpl));
    const afterFirst = await loadProject(bridge);
    assert.equal(afterFirst.status, 'consistent');
    assert.deepEqual(afterFirst.manifest.dependencies, {a: '^1.2.0'});
    assert.equal(afterFirst.lock.manifestDigest, await dependenciesDigest({a: '^1.2.0'}));
    await install(bridge, ['b@2.0.0', 'c@^3'], options(fetchImpl));
    assert.deepEqual(json(storage, manifestPath).dependencies, {a: '^1.2.0', b: '2.0.0', c: '^3'});
    assert.deepEqual(Object.keys(json(storage, lockPath).roots), ['a', 'b', 'c']);
    assert.equal((await loadProject(bridge)).status, 'consistent');
});

test('the three states of manifest and lock for install, install <pkg> and install --update', async () => {
    const {bridge, storage, pending} = await project('a');
    const {fetchImpl, requests} = registry(source());
    // consistent: install restores the locked graph, resolving and writing nothing.
    const manifestBefore = storage.get(manifestPath);
    const lockBefore = storage.get(lockPath);
    const cache = [...storage.keys()].filter(key => key.startsWith('.webfront/developer/packages/'));
    cache.forEach(key => storage.delete(key));
    const restored = await install(bridge, [], options(fetchImpl));
    assert.equal(restored.restored, 1);
    assert.ok(cache.every(key => storage.has(key)));
    assert.equal(storage.get(manifestPath), manifestBefore);
    assert.equal(storage.get(lockPath), lockBefore);
    assert.equal(requests.filter(url => !url.endsWith('.tgz')).length, 1, 'only the locked package is fetched, not a new resolution');
    // no lock: install resolves the manifest and creates the lock, leaving the manifest alone.
    storage.delete(lockPath);
    assert.equal((await loadProject(bridge)).status, 'unlocked');
    await install(bridge, [], options(fetchImpl));
    assert.equal((await loadProject(bridge)).status, 'consistent');
    assert.deepEqual(storage.get(manifestPath), manifestBefore);
    // desynchronized: every command says so and nothing is resolved implicitly.
    const edited = json(storage, manifestPath);
    edited.dependencies.b = '2.0.0';
    storage.set(manifestPath, encode(serializeManifest(edited)));
    const stale = await loadProject(bridge);
    assert.equal(stale.status, 'desynchronized');
    assert.match(syncNotice(stale), /wf install --update/);
    const lockStale = storage.get(lockPath);
    requests.length = 0;
    await assert.rejects(install(bridge, [], options(fetchImpl)), /wf install --update/);
    await assert.rejects(install(bridge, ['c'], options(fetchImpl)), /wf install --update/);
    assert.equal(requests.length, 0);
    assert.equal(storage.get(lockPath), lockStale);
    await install(bridge, ['--update'], options(fetchImpl));
    assert.equal((await loadProject(bridge)).status, 'consistent');
    assert.deepEqual(Object.keys(json(storage, lockPath).roots), ['a', 'b']);
    assert.equal(pending(), 0);
});

test('a changed archive is refused when restoring a locked graph', async () => {
    const {bridge, storage} = await project('a');
    [...storage.keys()].filter(key => key.startsWith('.webfront/developer/packages/')).forEach(key => storage.delete(key));
    const tampered = {a: {version: '1.2.0', bytes: pkg('a', '1.2.0', {description: 'changed'})}};
    await assert.rejects(install(bridge, [], options(registry(tampered).fetchImpl)), /Integrity check|integrity/);
});

test('two installations that read the same state cannot overwrite each other', async () => {
    const {bridge, storage, pending} = await project();
    const {fetchImpl} = registry(source());
    const results = await Promise.allSettled([install(bridge, ['a'], options(fetchImpl)), install(bridge, ['b'], options(fetchImpl))]);
    const refused = results.filter(result => result.status === 'rejected');
    assert.equal(refused.length, 1, 'exactly one publication is refused by the precondition');
    assert.match(refused[0].reason.message, /changed since it was read; run the command again/);
    assert.equal(pending(), 0, 'the refused installation leaves no staged transaction');
    assert.equal((await loadProject(bridge)).status, 'consistent');
    const winner = Object.keys(json(storage, manifestPath).dependencies);
    assert.equal(winner.length, 1);
    await install(bridge, [winner[0] === 'a' ? 'b' : 'a'], options(fetchImpl));
    assert.deepEqual(Object.keys(json(storage, manifestPath).dependencies).sort(), ['a', 'b']);
    assert.deepEqual(Object.keys(json(storage, lockPath).roots).sort(), ['a', 'b']);
    assert.equal((await loadProject(bridge)).status, 'consistent');
});

test('an invalid manifest or a lock without its digest is reported before anything is resolved', async () => {
    const {bridge, storage} = await project('a');
    const lock = json(storage, lockPath);
    delete lock.manifestDigest;
    storage.set(lockPath, encode(lock));
    assert.equal((await loadProject(bridge)).status, 'desynchronized');
    storage.set(manifestPath, encode({...minimal, format: 3}));
    await assert.rejects(loadProject(bridge), /format 3 is not supported/);
    storage.set(manifestPath, encode({...minimal, permissions: {network: true}}));
    await assert.rejects(install(bridge, [], options(registry(source()).fetchImpl)), /unknown permission/);
});

test('wf install --update takes the newest version within the ranges while installing a package keeps the others', async () => {
    const {bridge, storage} = await project('a');
    assert.equal(json(storage, lockPath).packages['a@1.2.0'].version, '1.2.0');
    const newer = {...source(), a: {version: '1.3.0', bytes: pkg('a', '1.3.0'), older: {'1.2.0': pkg('a', '1.2.0')}}};
    await install(bridge, ['b'], options(registry(newer).fetchImpl));
    assert.ok(json(storage, lockPath).packages['a@1.2.0'], 'installing another package leaves a at its locked version');
    assert.equal(json(storage, lockPath).packages['a@1.3.0'], undefined);
    await install(bridge, ['--update'], options(registry(newer).fetchImpl));
    assert.deepEqual(Object.keys(json(storage, lockPath).packages).sort(), ['a@1.3.0', 'b@2.0.0']);
    assert.deepEqual(json(storage, manifestPath).dependencies, {a: '^1.2.0', b: '^2.0.0'});
    assert.equal((await loadProject(bridge)).status, 'consistent');
});

test('a package installed by dist-tag keeps its version when another package is installed', async () => {
    const {bridge, storage} = await project();
    const old = {...source(), a: {version: '1.2.0', bytes: pkg('a', '1.2.0'), older: {'1.3.0': pkg('a', '1.3.0')}}};
    const beta = tags => options(tagged(registry(old).fetchImpl, tags));
    await install(bridge, ['a@beta'], beta({beta: '1.2.0'}));
    assert.deepEqual(json(storage, manifestPath).dependencies, {a: 'beta'});
    await install(bridge, ['b'], beta({beta: '1.3.0'}));
    assert.deepEqual(Object.keys(json(storage, lockPath).packages).sort(), ['a@1.2.0', 'b@2.0.0'], 'the tag moved but the lock did not');
    await install(bridge, ['--update'], beta({beta: '1.3.0'}));
    assert.deepEqual(Object.keys(json(storage, lockPath).packages).sort(), ['a@1.3.0', 'b@2.0.0']);
});
