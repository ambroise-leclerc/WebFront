// Tests of wf restore against the local registry fixture, served as plain files. Apart from node:test and
// node:assert, they use only browser APIs, so that they can move to wf test.
import {test, before} from 'node:test';
import assert from 'node:assert/strict';
import {install, initProject, restore, restorePackages, effectiveRegistry, cacheState, validateLock, lockPath, manifestPath, packageRoot} from '../environment/packages.bundle.mjs';
import {syntheticPackages, registryFiles} from './registry-fixture.mjs';
import {memoryBridge} from './memory-bridge.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const json = (storage, path) => JSON.parse(decoder.decode(storage.get(path)));
const cacheKeys = storage => [...storage.keys()].filter(key => key.startsWith(packageRoot + '/')).sort();
const snapshot = storage => new Map(storage);

const registries = {};
before(async () => {
    const packages = syntheticPackages();
    const genuine = packages.map(pkg => ({...pkg, tampered: false}));
    registries.local = (await registryFiles(packages)).files;
    // The same packages, with the tampered archive still matching its integrity: a lock can be created from it.
    registries.genuine = (await registryFiles(genuine)).files;
    // A mirror missing a package of the lock.
    registries.partial = (await registryFiles(genuine.filter(pkg => pkg.manifest.name !== 'webfront-fixture-lib'))).files;
});

// Serves the registry's files at an origin, as a static file server would; records every request.
function served(files, origin = 'https://registry.test') {
    const requests = [];
    const fetchImpl = async (url, {signal} = {}) => {
        signal?.throwIfAborted();
        requests.push(String(url));
        const {origin: requested, pathname} = new URL(url);
        if (requested !== origin) return new Response('', {status: 503});
        const bytes = files.get(decodeURIComponent(pathname.slice(1)));
        return bytes ? new Response(bytes) : new Response('', {status: 404});
    };
    return {fetchImpl, requests, registry: `${origin}/`};
}

const explicit = source => ({registry: source.registry, registryExplicit: true, fetchImpl: source.fetchImpl});

// A project locked to webfront-fixture-app@1.0.0 and webfront-fixture-lib@1.0.0, although lib 1.1.0 satisfies ^1.0.0.
async function lockedProject(files = registries.local, specs = ['webfront-fixture-lib@1.0.0', 'webfront-fixture-app@1.0.0']) {
    const memory = memoryBridge();
    await initProject(memory.bridge, [], {});
    for (const spec of specs) await install(memory.bridge, [spec], explicit(served(files)));
    return memory;
}

test('lock and empty cache: wf restore rebuilds the exact locked graph from the local registry', async () => {
    const {bridge, storage, pending} = await lockedProject();
    const lock = json(storage, lockPath);
    assert.deepEqual(Object.keys(lock.packages).sort(), ['webfront-fixture-app@1.0.0', 'webfront-fixture-lib@1.0.0']);
    const cached = cacheKeys(storage);
    assert.ok(cached.some(key => key.endsWith('.json')), 'each cached package has a record');
    cached.forEach(key => storage.delete(key));
    const before = snapshot(storage);
    const source = served(registries.local);
    const lines = [];
    const result = await restore(bridge, [], {...explicit(source), progress: line => lines.push(line)});
    assert.equal(result.restored, 2);
    assert.match(result.text, /Restored 2 of 2 packages \(0 already in the cache\)/);
    assert.deepEqual(cacheKeys(storage), cached, 'the cache is the one the installation wrote');
    assert.ok([...before].every(([key, bytes]) => storage.get(key) === bytes), 'manifest and lock are only read');
    assert.ok(source.requests.every(url => !url.includes('1.1.0')), 'no range is resolved again');
    assert.ok(source.requests.every(url => url.startsWith(source.registry)));
    assert.equal(lines[0], `Registry: ${source.registry} (application option)`);
    assert.equal(pending(), 0);
    // Idempotent: a valid cache is verified, not downloaded again.
    source.requests.length = 0;
    const again = await restore(bridge, [], explicit(source));
    assert.equal(again.restored, 0);
    assert.equal(again.verified, 2);
    assert.equal(source.requests.length, 0);
});

test('wf restore --offline checks the cache without any request and lists what is missing or corrupted', async () => {
    const {bridge, storage} = await lockedProject();
    const noNetwork = async () => { throw new Error('network access'); };
    const ok = await restore(bridge, ['--offline'], {fetchImpl: noNetwork});
    assert.match(ok.text, /Cache verified offline: 2 packages match/);
    const lock = json(storage, lockPath);
    const [app, lib] = ['webfront-fixture-app@1.0.0', 'webfront-fixture-lib@1.0.0'].map(id => lock.packages[id]);
    storage.delete(`${packageRoot}/${app.key}.json`);
    storage.set(`${packageRoot}/${lib.key}/index.js`, encoder.encode('changed'));
    const before = snapshot(storage);
    await assert.rejects(restore(bridge, ['--offline'], {fetchImpl: noNetwork}),
        error => /webfront-fixture-app@1\.0\.0 \(missing\)/.test(error.message) && /webfront-fixture-lib@1\.0\.0 \(corrupted\)/.test(error.message));
    assert.deepEqual(storage, before, 'an offline check writes nothing');
    assert.equal(await cacheState(bridge, app), 'missing');
    assert.equal(await cacheState(bridge, lib), 'corrupted');
    // Online, the same packages are downloaded again and the cache is valid.
    const result = await restore(bridge, [], explicit(served(registries.local)));
    assert.equal(result.restored, 2);
    assert.equal(await cacheState(bridge, lib), 'valid');
    // Only the files the lock lists are checked; a file added beside them is never read by a build.
    storage.set(`${packageRoot}/${lib.key}/extra.js`, encoder.encode(''));
    assert.equal(await cacheState(bridge, lib), 'valid');
});

test('the application registry takes precedence over the manifest one, which takes precedence over the default', async () => {
    const {bridge, storage} = await lockedProject();
    const manifest = json(storage, manifestPath);
    storage.set(manifestPath, encoder.encode(JSON.stringify({...manifest, registry: 'https://manifest.test/'})));
    cacheKeys(storage).forEach(key => storage.delete(key));
    const application = served(registries.local);
    const result = await restore(bridge, [], explicit(application));
    assert.deepEqual(result.registry, {url: 'https://registry.test/', source: 'application option'});
    assert.ok(application.requests.length && application.requests.every(url => url.startsWith('https://registry.test/')));
    cacheKeys(storage).forEach(key => storage.delete(key));
    const mirror = served(registries.local, 'https://manifest.test');
    const fromManifest = await restore(bridge, [], {registry: 'https://registry.test/', fetchImpl: mirror.fetchImpl});
    assert.deepEqual(fromManifest.registry, {url: 'https://manifest.test/', source: 'webfront.project.json'});
    assert.deepEqual(effectiveRegistry({}, {}), {url: 'https://registry.npmjs.org/', source: 'default'});
    assert.deepEqual(effectiveRegistry({registry: '/mirror/'}, {}), {url: '/mirror/', source: 'default'});
    await assert.rejects(restore(bridge, [], {registry: 'http://plain.test/', registryExplicit: true, fetchImpl: mirror.fetchImpl}), /HTTPS or the application origin/);
});

test('a changed archive or a package missing from the registry fails and keeps lock and valid cache intact', async () => {
    const {bridge, storage} = await lockedProject(registries.genuine, ['webfront-fixture-lib@1.0.0', 'webfront-fixture-tampered@1.0.0']);
    const lock = json(storage, lockPath);
    const tampered = lock.packages['webfront-fixture-tampered@1.0.0'];
    const lib = lock.packages['webfront-fixture-lib@1.0.0'];
    cacheKeys(storage).filter(key => key.includes('tampered')).forEach(key => storage.delete(key));
    const lockBytes = storage.get(lockPath);
    await assert.rejects(restore(bridge, [], explicit(served(registries.local))), /Integrity check failed/);
    assert.equal(storage.get(lockPath), lockBytes);
    assert.equal(await cacheState(bridge, tampered), 'missing');
    assert.equal(await cacheState(bridge, lib), 'valid');
    cacheKeys(storage).filter(key => key.includes('fixture-lib')).forEach(key => storage.delete(key));
    await assert.rejects(restore(bridge, [], explicit(served(registries.partial))), /webfront-fixture-lib: HTTP 404/);
    assert.equal(storage.get(lockPath), lockBytes);
});

test('an interrupted restore keeps the packages already restored and resumes with the others', async () => {
    const {bridge, storage, pending} = await lockedProject();
    cacheKeys(storage).forEach(key => storage.delete(key));
    const controller = new AbortController();
    const source = served(registries.local);
    let packuments = 0;
    const interrupting = async (url, options) => {
        if (!String(url).endsWith('.tgz') && ++packuments === 2) controller.abort(new Error('Interrupted'));
        return source.fetchImpl(url, options);
    };
    await assert.rejects(restore(bridge, [], {...explicit(source), fetchImpl: interrupting, signal: controller.signal}), /Interrupted/);
    assert.equal(pending(), 0, 'no staged transaction is left behind');
    const states = await Promise.all(Object.values(json(storage, lockPath).packages).map(node => cacheState(bridge, node)));
    assert.deepEqual(states.sort(), ['missing', 'valid']);
    const resumed = await restore(bridge, [], explicit(source));
    assert.equal(resumed.restored, 1);
    assert.equal(resumed.verified, 1);
});

// Unpacked sizes of the cached packages of a project, from the files the lock lists.
const sizeOf = (storage, node) => node.files.reduce((total, path) => total + storage.get(`${packageRoot}/${node.key}/${path}`).length, 0);

test('the bound of the unpacked graph counts the cached packages and is checked before publishing', async () => {
    const {bridge, storage} = await lockedProject();
    const lock = json(storage, lockPath);
    const [app, lib] = ['webfront-fixture-app@1.0.0', 'webfront-fixture-lib@1.0.0'].map(id => lock.packages[id]);
    const limit = sizeOf(storage, app) + sizeOf(storage, lib) - 1;
    cacheKeys(storage).filter(key => key.includes('fixture-lib')).forEach(key => storage.delete(key));
    const before = snapshot(storage);
    const source = served(registries.local);
    for (let attempt = 0; attempt < 2; attempt++) {
        await assert.rejects(restorePackages(bridge, lock, {...explicit(source), limit}), /exceed .* MiB unpacked/);
        assert.deepEqual(storage, before, 'nothing is published past the bound, and a second attempt keeps it');
    }
    const restored = await restorePackages(bridge, lock, {...explicit(source), limit: limit + 1});
    assert.equal(restored.restored, 1);
});

test('a cached file larger than the bound is a corruption, listed offline and repaired online', async () => {
    const {bridge, storage} = await lockedProject();
    const lock = json(storage, lockPath);
    const lib = lock.packages['webfront-fixture-lib@1.0.0'];
    const limit = 64 * 1024;
    storage.set(`${packageRoot}/${lib.key}/index.js`, new Uint8Array(3 * 256 * 1024));
    const reads = [];
    const counting = {cppFunction: name => async (...args) => {
        if (name.endsWith('.read')) reads.push(args[0]);
        return bridge.cppFunction(name)(...args);
    }};
    await assert.rejects(restorePackages(counting, lock, {offline: true, limit}), /webfront-fixture-lib@1\.0\.0 \(corrupted\)/);
    assert.equal(reads.filter(path => path.endsWith(`${lib.key}/index.js`)).length, 1, 'reading stops at the bound');
    const restored = await restorePackages(bridge, lock, {...explicit(served(registries.local)), limit});
    assert.equal(restored.restored, 1);
    assert.equal(await cacheState(bridge, lib), 'valid');
});

test('an invalid or absent lock is refused before any request or write', async () => {
    const {bridge, storage} = await lockedProject();
    const source = served(registries.local);
    for (const lock of ['{', JSON.stringify({format: 2, roots: {}, packages: {}})]) {
        storage.set(lockPath, encoder.encode(lock));
        const before = snapshot(storage);
        await assert.rejects(restore(bridge, [], explicit(source)));
        assert.deepEqual(storage, before);
    }
    storage.delete(lockPath);
    await assert.rejects(restore(bridge, [], explicit(source)), /No webfront-packages.lock.json: run wf install/);
    await assert.rejects(restore(bridge, ['--force'], explicit(source)), /Usage: wf restore \[--offline\]/);
    assert.equal(source.requests.length, 0);
    const packages = Object.fromEntries(Array.from({length: 513}, (_, i) => [`p${i}@1.0.0`, {}]));
    assert.throws(() => validateLock({format: 1, roots: {}, packages}), /exceeds 512 packages/);
});

test('wf install of a consistent lock repairs a corrupted cache and names its registry', async () => {
    const {bridge, storage} = await lockedProject();
    const lib = json(storage, lockPath).packages['webfront-fixture-lib@1.0.0'];
    storage.set(`${packageRoot}/${lib.key}/index.js`, encoder.encode('changed'));
    const lines = [];
    const result = await install(bridge, [], {...explicit(served(registries.local)), progress: line => lines.push(line)});
    assert.equal(result.restored, 1);
    assert.deepEqual(result.registry, {url: 'https://registry.test/', source: 'application option'});
    assert.ok(lines.includes('Registry: https://registry.test/ (application option)'));
    assert.equal(await cacheState(bridge, lib), 'valid');
});

test('the registry fixture is reproducible', async () => {
    const first = await registryFiles(syntheticPackages());
    const second = await registryFiles(syntheticPackages());
    assert.deepEqual(first, second);
    assert.equal(JSON.parse(new TextDecoder().decode(first.files.get('webfront-fixture-lib')))['dist-tags'].latest, '1.1.0');
});
