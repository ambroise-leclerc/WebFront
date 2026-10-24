// Specs of the project manifest, its agreement with the lock and the joint publication of both.
import {validateManifest, parseManifest, serializeManifest, manifestName, dependenciesDigest} from '../../../js/developer/environment/manifest.mjs';
import {install, initProject, loadProject, syncNotice, resolveEntry, embedTarget, lockPath, manifestPath} from '../../../js/developer/environment/packages.bundle.mjs';
import {compress} from '../../../js/developer/environment/embed.mjs';
import {tar} from '../../../js/developer/tools/archive.mjs';
import {memoryBridge} from '../memory-bridge.mjs';
import {rejects, throws, integrity} from '../support.mjs';

describe('The project manifest', () => {
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    const encode = value => encoder.encode(typeof value === 'string' ? value : JSON.stringify(value));
    const minimal = {format: 1, name: 'app', version: '0.1.0'};

    it('a valid manifest receives its defaults and keeps a stable serialization', () => {
        const manifest = validateManifest(minimal);
        expect(manifest.dependencies).toEqual({});
        expect(manifest.entries).toEqual({});
        const full = {...minimal, dependencies: {react: '^19.0.0', '@scope/lib': '>=1.2.0 <2', tool: 'latest'}, registry: 'https://registry.test/',
            scripts: {build: 'wf embed main'}, entries: {main: 'ui/main.tsx'},
            providers: {PiUI: {entry: 'main', output: 'generated/PiUI.hpp'}}, test: {specs: ['spec/a.mjs']},
            permissions: {files: ['ui'], publish: ['generated'], fetch: ['https://registry.npmjs.org']}};
        const text = serializeManifest(validateManifest(full));
        expect(JSON.parse(text)).toEqual(full);
        expect(serializeManifest(parseManifest(encode(text)))).toBe(text);
        expect(manifestName('apps/My App')).toBe('my-app');
        expect(manifestName('')).toBe('webfront-app');
    });

    it('invalid manifests are explicit errors, never silent fallbacks', () => {
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
            [{...minimal, registry: '/\\registry.test/'}, /registry must be an https URL or an absolute path/],
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
        for (const [value, message] of cases) throws(() => validateManifest(value), message);
        throws(() => parseManifest(encode('{')), /not valid JSON/);
    });

    it('the dependency digest ignores key order and changes with any range', async () => {
        const first = await dependenciesDigest({a: '^1.0.0', b: '2.0.0'});
        expect(first).toBe(await dependenciesDigest({b: '2.0.0', a: '^1.0.0'}));
        expect(first).not.toBe(await dependenciesDigest({a: '^1.0.0', b: '^2.0.0'}));
        expect(first).toMatch(/^[0-9a-f]{64}$/);
    });

    it('entries are named in the manifest or given as paths', () => {
        const manifest = validateManifest({...minimal, entries: {main: 'ui/main.tsx', other: 'ui/other.tsx'}});
        expect(resolveEntry(manifest, undefined, 'wf preview')).toBe('ui/main.tsx');
        expect(resolveEntry(manifest, 'other', 'wf preview')).toBe('ui/other.tsx');
        expect(resolveEntry(manifest, 'free/path.tsx', 'wf preview')).toBe('free/path.tsx');
        throws(() => resolveEntry(validateManifest(minimal), undefined, 'wf preview'), /entries.main/);
    });

    it('wf embed names the generated class after the provider, and refuses a file name that is not a C++ name', () => {
        const manifest = validateManifest({...minimal, entries: {main: 'ui/main.tsx'}, providers: {PiUI: {entry: 'main', output: 'generated/pi-ui.hpp'}}});
        expect(embedTarget(manifest, ['PiUI'])).toEqual({entry: 'ui/main.tsx', output: 'generated/pi-ui.hpp', className: 'PiUI'});
        expect(embedTarget(manifest, ['ui/main.tsx', '--output', 'generated/LifeUI.hpp']).className).toBe('LifeUI');
        throws(() => embedTarget(manifest, ['ui/main.tsx', '--output', 'generated/life-ui.hpp']), /not a C\+\+ class name/);
        throws(() => embedTarget(manifest, ['ui/main.tsx']), /--output/);
    });

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
            const described = Object.fromEntries(await Promise.all(Object.entries(versions).map(async ([version, bytes]) =>
                [version, {dist: {tarball: `https://registry.test/${path}@${version}.tgz`, integrity: await integrity(bytes)}}])));
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

    it('wf init creates the root manifest every command reads, and never replaces it', async () => {
        const {bridge, storage} = memoryBridge();
        expect(await initProject(bridge, [], {})).toMatch(/Created webfront.project.json/);
        expect(json(storage, manifestPath)).toEqual({format: 1, name: 'webfront-app', version: '0.1.0', dependencies: {}});
        expect((await loadProject(bridge)).status).toBe('unlocked');
        await rejects(initProject(bridge, [], {}), /already exists/);
        await rejects(initProject(bridge, ['a', 'b'], {}), /Usage/);
        const named = memoryBridge();
        await initProject(named.bridge, ['my-app'], {});
        expect(json(named.storage, manifestPath).name).toBe('my-app');
        await rejects(initProject(memoryBridge().bridge, ['Bad Name'], {}), /name must be/);
    });

    it('a project locked before the manifest keeps its locked graph through wf init and wf install', async () => {
        const {bridge, storage, pending} = await project('a', 'b@2.0.0', 'c@^3');
        const lock = json(storage, lockPath);
        delete lock.manifestDigest;
        storage.set(lockPath, encode(lock));
        storage.delete(manifestPath);
        expect(await initProject(bridge, [], {})).toMatch(/with the 3 dependencies of webfront-packages.lock.json/);
        expect(json(storage, manifestPath).dependencies).toEqual({a: '^1.2.0', b: '^2.0.0', c: '^3'});
        expect((await loadProject(bridge)).status).toBe('legacy');
        expect(syncNotice(await loadProject(bridge))).toBe('');
        // A newer a is published: wf install keeps the locked version and only records the digest.
        const newer = {...source(), a: {version: '1.3.0', bytes: pkg('a', '1.3.0'), older: {'1.2.0': pkg('a', '1.2.0')}}};
        await install(bridge, [], options(registry(newer).fetchImpl));
        const adopted = json(storage, lockPath);
        expect(Object.keys(adopted.packages).sort()).toEqual(['a@1.2.0', 'b@2.0.0', 'c@3.1.0']);
        expect(adopted.manifestDigest).toBe(await dependenciesDigest({a: '^1.2.0', b: '^2.0.0', c: '^3'}));
        expect((await loadProject(bridge)).status).toBe('consistent');
        expect(pending()).toBe(0);
    });

    it('a legacy lock also stays usable for wf install <pkg>, and one that does not match is desynchronized', async () => {
        const {bridge, storage} = await project('a');
        const lock = json(storage, lockPath);
        delete lock.manifestDigest;
        storage.set(lockPath, encode(lock));
        const newer = {...source(), a: {version: '1.3.0', bytes: pkg('a', '1.3.0'), older: {'1.2.0': pkg('a', '1.2.0')}}};
        await install(bridge, ['b'], options(registry(newer).fetchImpl));
        expect(Object.keys(json(storage, lockPath).packages).sort()).toEqual(['a@1.2.0', 'b@2.0.0']);
        expect((await loadProject(bridge)).status).toBe('consistent');
        const stale = json(storage, lockPath);
        delete stale.manifestDigest;
        storage.set(lockPath, encode(stale));
        storage.set(manifestPath, encode({...minimal, dependencies: {a: '^2.0.0', b: '^2.0.0'}}));
        expect((await loadProject(bridge)).status).toBe('desynchronized');
    });

    it('wf install refuses a range the manifest cannot hold before resolving, and writes only what the user wrote', async () => {
        const {bridge, storage} = await project();
        const {fetchImpl, requests} = registry(source());
        const before = storage.get(manifestPath);
        for (const spec of ['a@next_v2', `a@${'1'.repeat(129)}`]) await rejects(install(bridge, [spec], options(fetchImpl)), /invalid version range/);
        expect(requests.length).toBe(0);
        expect(storage.get(manifestPath)).toBe(before);
        await install(bridge, ['a@'], options(fetchImpl));
        expect(json(storage, manifestPath)).toEqual({format: 1, name: 'webfront-app', version: '0.1.0', dependencies: {a: '^1.2.0'}});
        expect((await loadProject(bridge)).manifest.entries).withContext('the defaults are still applied when reading').toEqual({});
    });

    it('wf install needs a manifest, then publishes manifest and lock together', async () => {
        const {bridge, storage} = memoryBridge();
        const {fetchImpl} = registry(source());
        await rejects(install(bridge, ['a'], options(fetchImpl)), /run wf init/);
        await initProject(bridge, [], {});
        expect((await loadProject(bridge)).status).toBe('unlocked');
        await install(bridge, ['a'], options(fetchImpl));
        const afterFirst = await loadProject(bridge);
        expect(afterFirst.status).toBe('consistent');
        expect(afterFirst.manifest.dependencies).toEqual({a: '^1.2.0'});
        expect(afterFirst.lock.manifestDigest).toBe(await dependenciesDigest({a: '^1.2.0'}));
        await install(bridge, ['b@2.0.0', 'c@^3'], options(fetchImpl));
        expect(json(storage, manifestPath).dependencies).toEqual({a: '^1.2.0', b: '2.0.0', c: '^3'});
        expect(Object.keys(json(storage, lockPath).roots)).toEqual(['a', 'b', 'c']);
        expect((await loadProject(bridge)).status).toBe('consistent');
    });

    it('the three states of manifest and lock for install, install <pkg> and install --update', async () => {
        const {bridge, storage, pending} = await project('a');
        const {fetchImpl, requests} = registry(source());
        // consistent: install restores the locked graph, resolving and writing nothing.
        const manifestBefore = storage.get(manifestPath);
        const lockBefore = storage.get(lockPath);
        const cache = [...storage.keys()].filter(key => key.startsWith('.webfront/developer/packages/'));
        cache.forEach(key => storage.delete(key));
        const restored = await install(bridge, [], options(fetchImpl));
        expect(restored.restored).toBe(1);
        expect(cache.every(key => storage.has(key))).toBeTruthy();
        expect(storage.get(manifestPath)).toBe(manifestBefore);
        expect(storage.get(lockPath)).toBe(lockBefore);
        expect(requests.filter(url => !url.endsWith('.tgz')).length).withContext('only the locked package is fetched, not a new resolution').toBe(1);
        // no lock: install resolves the manifest and creates the lock, leaving the manifest alone.
        storage.delete(lockPath);
        expect((await loadProject(bridge)).status).toBe('unlocked');
        await install(bridge, [], options(fetchImpl));
        expect((await loadProject(bridge)).status).toBe('consistent');
        expect(storage.get(manifestPath)).toEqual(manifestBefore);
        // desynchronized: every command says so and nothing is resolved implicitly.
        const edited = json(storage, manifestPath);
        edited.dependencies.b = '2.0.0';
        storage.set(manifestPath, encode(serializeManifest(edited)));
        const stale = await loadProject(bridge);
        expect(stale.status).toBe('desynchronized');
        expect(syncNotice(stale)).toMatch(/wf install --update/);
        const lockStale = storage.get(lockPath);
        requests.length = 0;
        await rejects(install(bridge, [], options(fetchImpl)), /wf install --update/);
        await rejects(install(bridge, ['c'], options(fetchImpl)), /wf install --update/);
        expect(requests.length).toBe(0);
        expect(storage.get(lockPath)).toBe(lockStale);
        await install(bridge, ['--update'], options(fetchImpl));
        expect((await loadProject(bridge)).status).toBe('consistent');
        expect(Object.keys(json(storage, lockPath).roots)).toEqual(['a', 'b']);
        expect(pending()).toBe(0);
    });

    it('a changed archive is refused when restoring a locked graph', async () => {
        const {bridge, storage} = await project('a');
        [...storage.keys()].filter(key => key.startsWith('.webfront/developer/packages/')).forEach(key => storage.delete(key));
        const tampered = {a: {version: '1.2.0', bytes: pkg('a', '1.2.0', {description: 'changed'})}};
        await rejects(install(bridge, [], options(registry(tampered).fetchImpl)), /Integrity check|integrity/);
    });

    it('two installations that read the same state cannot overwrite each other', async () => {
        const {bridge, storage, pending} = await project();
        const {fetchImpl} = registry(source());
        const results = await Promise.allSettled([install(bridge, ['a'], options(fetchImpl)), install(bridge, ['b'], options(fetchImpl))]);
        const refused = results.filter(result => result.status === 'rejected');
        expect(refused.length).withContext('exactly one publication is refused by the precondition').toBe(1);
        expect(refused[0].reason.message).toMatch(/changed since it was read; run the command again/);
        expect(pending()).withContext('the refused installation leaves no staged transaction').toBe(0);
        expect((await loadProject(bridge)).status).toBe('consistent');
        const winner = Object.keys(json(storage, manifestPath).dependencies);
        expect(winner.length).toBe(1);
        await install(bridge, [winner[0] === 'a' ? 'b' : 'a'], options(fetchImpl));
        expect(Object.keys(json(storage, manifestPath).dependencies).sort()).toEqual(['a', 'b']);
        expect(Object.keys(json(storage, lockPath).roots).sort()).toEqual(['a', 'b']);
        expect((await loadProject(bridge)).status).toBe('consistent');
    });

    it('an invalid manifest is reported before anything is resolved', async () => {
        const {bridge, storage} = await project('a');
        storage.set(manifestPath, encode({...minimal, format: 3}));
        await rejects(loadProject(bridge), /format 3 is not supported/);
        storage.set(manifestPath, encode({...minimal, permissions: {network: true}}));
        await rejects(install(bridge, [], options(registry(source()).fetchImpl)), /unknown permission/);
    });

    it('wf install --update takes the newest version within the ranges while installing a package keeps the others', async () => {
        const {bridge, storage} = await project('a');
        expect(json(storage, lockPath).packages['a@1.2.0'].version).toBe('1.2.0');
        const newer = {...source(), a: {version: '1.3.0', bytes: pkg('a', '1.3.0'), older: {'1.2.0': pkg('a', '1.2.0')}}};
        await install(bridge, ['b'], options(registry(newer).fetchImpl));
        expect(json(storage, lockPath).packages['a@1.2.0']).withContext('installing another package leaves a at its locked version').toBeTruthy();
        expect(json(storage, lockPath).packages['a@1.3.0']).toBe(undefined);
        await install(bridge, ['--update'], options(registry(newer).fetchImpl));
        expect(Object.keys(json(storage, lockPath).packages).sort()).toEqual(['a@1.3.0', 'b@2.0.0']);
        expect(json(storage, manifestPath).dependencies).toEqual({a: '^1.2.0', b: '^2.0.0'});
        expect((await loadProject(bridge)).status).toBe('consistent');
    });

    it('a package installed by dist-tag keeps its version when another package is installed', async () => {
        const {bridge, storage} = await project();
        const old = {...source(), a: {version: '1.2.0', bytes: pkg('a', '1.2.0'), older: {'1.3.0': pkg('a', '1.3.0')}}};
        const beta = tags => options(tagged(registry(old).fetchImpl, tags));
        await install(bridge, ['a@beta'], beta({beta: '1.2.0'}));
        expect(json(storage, manifestPath).dependencies).toEqual({a: 'beta'});
        await install(bridge, ['b'], beta({beta: '1.3.0'}));
        expect(Object.keys(json(storage, lockPath).packages).sort()).withContext('the tag moved but the lock did not').toEqual(['a@1.2.0', 'b@2.0.0']);
        await install(bridge, ['--update'], beta({beta: '1.3.0'}));
        expect(Object.keys(json(storage, lockPath).packages).sort()).toEqual(['a@1.3.0', 'b@2.0.0']);
    });
});
