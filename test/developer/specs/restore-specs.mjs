// Specs of wf restore against the local registry fixture, served as plain files.
import {install, initProject, restore, restorePackages, effectiveRegistry, usableRegistry, cacheState, validateLock, lockPath, manifestPath, packageRoot} from '../../../js/developer/environment/packages.bundle.mjs';
import {syntheticPackages, registryFiles} from '../../web/registry/fixture.mjs';
import {memoryBridge} from '../memory-bridge.mjs';
import {rejects, throws} from '../support.mjs';

describe('wf restore', () => {
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    const json = (storage, path) => JSON.parse(decoder.decode(storage.get(path)));
    const cacheKeys = storage => [...storage.keys()].filter(key => key.startsWith(packageRoot + '/')).sort();
    const snapshot = storage => new Map(storage);

    const registries = {};
    beforeAll(async () => {
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

    it('lock and empty cache: wf restore rebuilds the exact locked graph from the local registry', async () => {
        const {bridge, storage, pending} = await lockedProject();
        const lock = json(storage, lockPath);
        expect(Object.keys(lock.packages).sort()).toEqual(['webfront-fixture-app@1.0.0', 'webfront-fixture-lib@1.0.0']);
        const cached = cacheKeys(storage);
        expect(cached.some(key => key.endsWith('.json'))).withContext('each cached package has a record').toBeTruthy();
        cached.forEach(key => storage.delete(key));
        const before = snapshot(storage);
        const source = served(registries.local);
        const lines = [];
        const result = await restore(bridge, [], {...explicit(source), progress: line => lines.push(line)});
        expect(result.restored).toBe(2);
        expect(result.text).toMatch(/Restored 2 of 2 packages \(0 already in the cache\)/);
        expect(cacheKeys(storage)).withContext('the cache is the one the installation wrote').toEqual(cached);
        expect([...before].every(([key, bytes]) => storage.get(key) === bytes)).withContext('manifest and lock are only read').toBeTruthy();
        expect(source.requests.every(url => !url.includes('1.1.0'))).withContext('no range is resolved again').toBeTruthy();
        expect(source.requests.every(url => url.startsWith(source.registry))).toBeTruthy();
        expect(lines[0]).toBe(`Registry: ${source.registry} (application option)`);
        expect(pending()).toBe(0);
        // Idempotent: a valid cache is verified, not downloaded again.
        source.requests.length = 0;
        const again = await restore(bridge, [], explicit(source));
        expect(again.restored).toBe(0);
        expect(again.verified).toBe(2);
        expect(source.requests.length).toBe(0);
    });

    it('wf restore --offline checks the cache without any request and lists what is missing or corrupted', async () => {
        const {bridge, storage} = await lockedProject();
        const noNetwork = async () => { throw new Error('network access'); };
        const ok = await restore(bridge, ['--offline'], {fetchImpl: noNetwork});
        expect(ok.text).toMatch(/Cache verified offline: 2 packages match/);
        const lock = json(storage, lockPath);
        const [app, lib] = ['webfront-fixture-app@1.0.0', 'webfront-fixture-lib@1.0.0'].map(id => lock.packages[id]);
        storage.delete(`${packageRoot}/${app.key}.json`);
        storage.set(`${packageRoot}/${lib.key}/index.js`, encoder.encode('changed'));
        const before = snapshot(storage);
        await rejects(restore(bridge, ['--offline'], {fetchImpl: noNetwork}),
            error => /webfront-fixture-app@1\.0\.0 \(missing\)/.test(error.message) && /webfront-fixture-lib@1\.0\.0 \(corrupted\)/.test(error.message));
        expect(storage).withContext('an offline check writes nothing').toEqual(before);
        expect(await cacheState(bridge, app)).toBe('missing');
        expect(await cacheState(bridge, lib)).toBe('corrupted');
        // Online, the same packages are downloaded again and the cache is valid.
        const result = await restore(bridge, [], explicit(served(registries.local)));
        expect(result.restored).toBe(2);
        expect(await cacheState(bridge, lib)).toBe('valid');
        // Only the files the lock lists are checked; a file added beside them is never read by a build.
        storage.set(`${packageRoot}/${lib.key}/extra.js`, encoder.encode(''));
        expect(await cacheState(bridge, lib)).toBe('valid');
    });

    it('the application registry takes precedence over the manifest one, which takes precedence over the default', async () => {
        const {bridge, storage} = await lockedProject();
        const manifest = json(storage, manifestPath);
        storage.set(manifestPath, encoder.encode(JSON.stringify({...manifest, registry: 'https://manifest.test/'})));
        cacheKeys(storage).forEach(key => storage.delete(key));
        const application = served(registries.local);
        const result = await restore(bridge, [], explicit(application));
        expect(result.registry).toEqual({url: 'https://registry.test/', source: 'application option'});
        expect(application.requests.length && application.requests.every(url => url.startsWith('https://registry.test/'))).toBeTruthy();
        cacheKeys(storage).forEach(key => storage.delete(key));
        const mirror = served(registries.local, 'https://manifest.test');
        const fromManifest = await restore(bridge, [], {registry: 'https://registry.test/', fetchImpl: mirror.fetchImpl});
        expect(fromManifest.registry).toEqual({url: 'https://manifest.test/', source: 'webfront.project.json'});
        expect(effectiveRegistry({}, {})).toEqual({url: 'https://registry.npmjs.org/', source: 'default'});
        expect(effectiveRegistry({registry: '/mirror/'}, {})).toEqual({url: '/mirror/', source: 'default'});
        await rejects(restore(bridge, [], {registry: 'http://plain.test/', registryExplicit: true, fetchImpl: mirror.fetchImpl}), /HTTPS or the application origin/);
    });

    it('a registry path never leaves the application origin, however a URL parser reads it', () => {
        // On an HTTPS page, "/\\host" resolves to https://host/, which an HTTPS check alone would accept; here the page is the
        // application's own, and no such path may leave its origin either.
        expect(usableRegistry({registry: '/mirror/', registryExplicit: true}).url).toBe('/mirror/');
        for (const registry of ['//registry.test/', '/\\registry.test/', '/\\\\registry.test/', 'http://registry.test/'])
            throws(() => usableRegistry({registry, registryExplicit: true}), /HTTPS or the application origin/);
        expect(usableRegistry({registry: 'https://registry.test/', registryExplicit: true}).url).toBe('https://registry.test/');
    });

    it('a bridge failure while reading a record is reported, not taken for a corrupted cache', async () => {
        const {bridge} = await lockedProject();
        const failing = {cppFunction: name => async (...args) => {
            if (name.endsWith('.read') && args[0].endsWith('.json') && args[0].includes('.webfront/')) throw new Error('Bridge disconnected');
            return bridge.cppFunction(name)(...args);
        }};
        const source = served(registries.local);
        await rejects(restore(failing, ['--offline'], {}), /Bridge disconnected/);
        await rejects(restore(failing, [], explicit(source)), /Bridge disconnected/);
        expect(source.requests.length).withContext('a valid package is not downloaded again').toBe(0);
    });

    it('a changed archive or a package missing from the registry fails and keeps lock and valid cache intact', async () => {
        const {bridge, storage} = await lockedProject(registries.genuine, ['webfront-fixture-lib@1.0.0', 'webfront-fixture-tampered@1.0.0']);
        const lock = json(storage, lockPath);
        const tampered = lock.packages['webfront-fixture-tampered@1.0.0'];
        const lib = lock.packages['webfront-fixture-lib@1.0.0'];
        cacheKeys(storage).filter(key => key.includes('tampered')).forEach(key => storage.delete(key));
        const lockBytes = storage.get(lockPath);
        await rejects(restore(bridge, [], explicit(served(registries.local))), /Integrity check failed/);
        expect(storage.get(lockPath)).toBe(lockBytes);
        expect(await cacheState(bridge, tampered)).toBe('missing');
        expect(await cacheState(bridge, lib)).toBe('valid');
        cacheKeys(storage).filter(key => key.includes('fixture-lib')).forEach(key => storage.delete(key));
        await rejects(restore(bridge, [], explicit(served(registries.partial))), /webfront-fixture-lib: HTTP 404/);
        expect(storage.get(lockPath)).toBe(lockBytes);
    });

    it('an interrupted restore keeps the packages already restored and resumes with the others', async () => {
        const {bridge, storage, pending} = await lockedProject();
        cacheKeys(storage).forEach(key => storage.delete(key));
        const controller = new AbortController();
        const source = served(registries.local);
        let packuments = 0;
        const interrupting = async (url, options) => {
            if (!String(url).endsWith('.tgz') && ++packuments === 2) controller.abort(new Error('Interrupted'));
            return source.fetchImpl(url, options);
        };
        await rejects(restore(bridge, [], {...explicit(source), fetchImpl: interrupting, signal: controller.signal}), /Interrupted/);
        expect(pending()).withContext('no staged transaction is left behind').toBe(0);
        const states = await Promise.all(Object.values(json(storage, lockPath).packages).map(node => cacheState(bridge, node)));
        expect(states.sort()).toEqual(['missing', 'valid']);
        const resumed = await restore(bridge, [], explicit(source));
        expect(resumed.restored).toBe(1);
        expect(resumed.verified).toBe(1);
    });

    // Unpacked sizes of the cached packages of a project, from the files the lock lists.
    const sizeOf = (storage, node) => node.files.reduce((total, path) => total + storage.get(`${packageRoot}/${node.key}/${path}`).length, 0);

    it('the bound of the unpacked graph counts the cached packages and is checked before publishing', async () => {
        const {bridge, storage} = await lockedProject();
        const lock = json(storage, lockPath);
        const [app, lib] = ['webfront-fixture-app@1.0.0', 'webfront-fixture-lib@1.0.0'].map(id => lock.packages[id]);
        const limit = sizeOf(storage, app) + sizeOf(storage, lib) - 1;
        cacheKeys(storage).filter(key => key.includes('fixture-lib')).forEach(key => storage.delete(key));
        const before = snapshot(storage);
        const source = served(registries.local);
        for (let attempt = 0; attempt < 2; attempt++) {
            await rejects(restorePackages(bridge, lock, {...explicit(source), limit}), /exceed .* MiB unpacked/);
            expect(storage).withContext('nothing is published past the bound, and a second attempt keeps it').toEqual(before);
        }
        const restored = await restorePackages(bridge, lock, {...explicit(source), limit: limit + 1});
        expect(restored.restored).toBe(1);
    });

    it('a cached file larger than the bound is a corruption, listed offline and repaired online', async () => {
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
        await rejects(restorePackages(counting, lock, {offline: true, limit}), /webfront-fixture-lib@1\.0\.0 \(corrupted\)/);
        expect(reads.filter(path => path.endsWith(`${lib.key}/index.js`)).length).withContext('reading stops at the bound').toBe(1);
        const restored = await restorePackages(bridge, lock, {...explicit(served(registries.local)), limit});
        expect(restored.restored).toBe(1);
        expect(await cacheState(bridge, lib)).toBe('valid');
    });

    it('an invalid or absent lock is refused before any request or write', async () => {
        const {bridge, storage} = await lockedProject();
        const source = served(registries.local);
        for (const lock of ['{', JSON.stringify({format: 2, roots: {}, packages: {}})]) {
            storage.set(lockPath, encoder.encode(lock));
            const before = snapshot(storage);
            await rejects(restore(bridge, [], explicit(source)));
            expect(storage).toEqual(before);
        }
        storage.delete(lockPath);
        await rejects(restore(bridge, [], explicit(source)), /No webfront-packages.lock.json: run wf install/);
        await rejects(restore(bridge, ['--force'], explicit(source)), /Usage: wf restore \[--offline\]/);
        expect(source.requests.length).toBe(0);
        const packages = Object.fromEntries(Array.from({length: 513}, (_, i) => [`p${i}@1.0.0`, {}]));
        throws(() => validateLock({format: 1, roots: {}, packages}), /exceeds 512 packages/);
    });

    it('wf install of a consistent lock repairs a corrupted cache and names its registry', async () => {
        const {bridge, storage} = await lockedProject();
        const lib = json(storage, lockPath).packages['webfront-fixture-lib@1.0.0'];
        storage.set(`${packageRoot}/${lib.key}/index.js`, encoder.encode('changed'));
        const lines = [];
        const result = await install(bridge, [], {...explicit(served(registries.local)), progress: line => lines.push(line)});
        expect(result.restored).toBe(1);
        expect(result.registry).toEqual({url: 'https://registry.test/', source: 'application option'});
        expect(lines.includes('Registry: https://registry.test/ (application option)')).toBeTruthy();
        expect(await cacheState(bridge, lib)).toBe('valid');
    });

    it('the registry fixture is reproducible', async () => {
        const first = await registryFiles(syntheticPackages());
        const second = await registryFiles(syntheticPackages());
        expect(first).toEqual(second);
        expect(JSON.parse(new TextDecoder().decode(first.files.get('webfront-fixture-lib')))['dist-tags'].latest).toBe('1.1.0');
    });
});
