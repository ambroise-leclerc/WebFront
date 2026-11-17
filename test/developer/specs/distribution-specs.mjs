// Specs of the developer distribution: the starters, the Pages staging and the pinned bootstrap header.
import {tar} from '../../../js/developer/tools/archive.mjs';
import {bootstrapHeader, versionPath, distributionFiles, customDomain} from '../../../js/developer/tools/distribution.mjs';
import {pinOrigin, precedes} from '../../../js/developer/scripts/pin.mjs';
import stageSite from '../../../js/developer/scripts/site.mjs';
import {compress} from '../../../js/developer/environment/embed.mjs';
import {createStarter} from '../../../js/developer/environment/starter.mjs';
import {rejects, throws, integrity} from '../support.mjs';

describe('The developer distribution', () => {
    const encode = value => new TextEncoder().encode(typeof value === 'string' ? value : JSON.stringify(value));
    const decode = bytes => new TextDecoder().decode(bytes);

    const recipe = (fields = {}) => ({format: 1, name: 'life-starter', version: '0.1.0', dependencies: {react: '19.3.1'},
        entries: {main: 'main.tsx'}, ...fields});

    function starter(overrides = {}) {
        return compress(tar(new Map(Object.entries({
            'package/main.tsx': encode('export default function Life() {}'),
            'package/webfront.project.json': encode(recipe()),
            ...overrides,
        }))));
    }

    it('starter integrity, recipes and archive paths are checked before any project writes', async () => {
        const bytes = starter();
        const calls = [];
        const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return 'transaction'; }};
        const config = {starters: {life: {url: 'https://pages.test/developer/0.1.0/life-starter.tgz', integrity: await integrity(bytes)}}};
        const fetchImpl = async () => new Response(bytes);
        const help = await createStarter(bridge, config, ['life', 'ui'], {fetchImpl});
        expect(help).toMatch(/wf init\nwf install react@19.3.1/);
        expect(help).toMatch(/wf preview ui\/main.tsx/);
        expect(calls[0][0]).toBe('webfront.developer.beginNew');
        expect(calls.at(-1)[0]).toBe('webfront.developer.commit');
        calls.length = 0;
        await rejects(createStarter(bridge, {starters: {life: {...config.starters.life, integrity: await integrity(encode('bad'))}}}, ['life', 'ui'], {fetchImpl}), /Integrity check/);
        expect(calls.length).toBe(0);
        for (const bytes of [starter({'package/../escape': encode('bad')}), starter({'package/webfront.project.json': encode(recipe({dependencies: {react: 'latest'}}))})]) {
            await rejects(createStarter(bridge, {starters: {life: {...config.starters.life, integrity: await integrity(bytes)}}}, ['life', 'ui'], {fetchImpl: async () => new Response(bytes)}));
            expect(calls.length).toBe(0);
        }
    });

    it('a starter without dependency lists no installation, and an unknown name is answered with the starters', async () => {
        const bytes = starter({'package/main.ts': encode('export {};'),
            'package/webfront.project.json': encode(recipe({name: 'app-starter', dependencies: {}, entries: {main: 'main.ts'}}))});
        const bridge = {cppFunction: () => async () => 'transaction'};
        const config = {starters: {life: {url: '/life-starter.tgz', integrity: await integrity(bytes)}, app: {url: '/app-starter.tgz', integrity: await integrity(bytes)}}};
        const help = await createStarter(bridge, config, ['app', 'ui'], {fetchImpl: async () => new Response(bytes)});
        expect(help).toBe('Created ui.\nwf init\nwf dev ui/main.ts\nwf preview ui/main.ts\nwf embed ui/main.ts --output generated/AppUI.hpp');
        // The argument is easily taken for the name of the project (#561): the answer says what it is and what exists.
        await rejects(createStarter(bridge, config, ['my-ui', 'ui'], {fetchImpl: async () => new Response(bytes)}),
            /Unknown pinned starter: my-ui\. wf create takes the name of a starter, not of your project; available: app, life/);
        await rejects(createStarter(bridge, config, ['constructor', 'ui'], {fetchImpl: async () => new Response(bytes)}), /Unknown pinned starter: constructor/);
        await rejects(createStarter(bridge, {}, ['app', 'ui']), /Unknown pinned starter: app\. wf create takes the name of a starter, not of your project$/);
    });

    it('starter replacement refusal aborts the publication rather than overwriting files', async () => {
        const bytes = starter();
        const calls = [];
        const bridge = {cppFunction: name => async () => {
            calls.push(name);
            if (name.endsWith('.commit')) throw new Error('Starter destination already exists');
            return 'transaction';
        }};
        await rejects(createStarter(bridge, {starters: {life: {url: '/starter.tgz', integrity: await integrity(bytes)}}}, ['life', 'ui'], {fetchImpl: async () => new Response(bytes)}), /already exists/);
        expect(calls.at(-1)).toBe('webfront.developer.abort');
    });

    // The project a wf script sees, over a map of files: directories are the prefixes of the paths, listed with a trailing
    // slash, and a directory that holds no file cannot be listed or read, like with developer::Store.
    function memoryProject(storage = new Map()) {
        const directory = path => path ? path + '/' : '';
        return {storage,
            read: async path => {
                if (!storage.has(path)) throw new Error(`Cannot read project file: ${path}`);
                return storage.get(path);
            },
            readText: async path => decode(await storage.get(path)),
            write: async (path, data) => { storage.set(path, typeof data === 'string' ? encode(data) : data); },
            list: async path => {
                const prefix = directory(path.replace(/\/$/, ''));
                const names = new Set([...storage.keys()].filter(key => key.startsWith(prefix))
                    .map(key => key.slice(prefix.length)).map(rest => rest.includes('/') ? rest.split('/')[0] + '/' : rest));
                if (!names.size) throw new Error(`Cannot list project directory: ${path}`);
                return [...names].sort();
            }};
    }

    it('Pages staging retains older versions and refuses different bytes under an existing version', async () => {
        const project = memoryProject();
        await project.write('js/developer/tools/distribution.json', JSON.stringify({version: '0.1.0', baseUrl: 'https://owner.github.io/WebFront/'}));
        const environment = encode('environment');
        const starterBytes = encode('starter');
        const write = async version => {
            const manifest = {version, integrity: await integrity(environment), starters: {sample: {archive: 'sample-starter.tgz', integrity: await integrity(starterBytes)},
                ...(version !== '0.1.0' ? {life: {archive: 'life-starter.tgz', integrity: await integrity(starterBytes)}} : {})}};
            await project.write('distribution/environment.tgz', environment);
            await project.write('distribution/sample-starter.tgz', starterBytes);
            if (version !== '0.1.0') await project.write('distribution/life-starter.tgz', starterBytes);
            await project.write('distribution/provision.json', JSON.stringify(manifest));
            await project.write('distribution/DeveloperReleaseData.hpp', bootstrapHeader(version, 'https://owner.github.io/WebFront/', manifest.integrity));
        };
        const stage = () => stageSite({args: ['distribution', 'site'], project, console: {log() {}}});
        await write('0.1.0');
        await stage();
        await stage();
        await write('0.2.0');
        await stage();
        expect(await project.readText('site/developer/0.1.0/environment.tgz')).toBe('environment');
        expect(project.storage.has('site/CNAME')).toBe(false);
        expect(await project.readText('site/index.html')).toMatch(/developer\/0.2.0\/life-starter.tgz/);
        expect(await project.readText('site/developer/0.2.0/life-starter.tgz')).toBe('starter');
        await write('0.3.0');
        await project.write('distribution/life-starter.tgz', 'packed-starter');
        const manifest = JSON.parse(await project.readText('distribution/provision.json'));
        manifest.starters.life.integrity = await integrity(encode('packed-starter'));
        await project.write('distribution/provision.json', JSON.stringify(manifest));
        await stage();
        expect(await project.readText('site/developer/0.1.0/environment.tgz')).toBe('environment');
        expect(await project.readText('site/developer/0.2.0/life-starter.tgz')).toBe('starter');
        expect(await project.readText('site/developer/0.3.0/life-starter.tgz')).toBe('packed-starter');
        await project.write('distribution/DeveloperReleaseData.hpp', 'changed');
        await rejects(stage(), /increment the distribution version/);
        expect(await project.readText('site/developer/0.2.0/DeveloperReleaseData.hpp')).toMatch(/namespace webfront/);
        await project.write('distribution/environment.tgz', 'corrupt');
        await rejects(stage(), /checksum mismatch/);
    });

    it('Pages staging writes the CNAME file of a custom domain on every publication', async () => {
        const project = memoryProject();
        await project.write('js/developer/tools/distribution.json', JSON.stringify({version: '0.1.0', baseUrl: 'https://dist.example.test/'}));
        const environment = encode('environment');
        const starterBytes = encode('starter');
        const manifest = {version: '0.1.0', integrity: await integrity(environment), starters: {life: {archive: 'life-starter.tgz', integrity: await integrity(starterBytes)}}};
        await project.write('distribution/environment.tgz', environment);
        await project.write('distribution/life-starter.tgz', starterBytes);
        await project.write('distribution/provision.json', JSON.stringify(manifest));
        await project.write('distribution/DeveloperReleaseData.hpp', bootstrapHeader('0.1.0', 'https://dist.example.test/', manifest.integrity));
        const stage = () => stageSite({args: ['distribution', 'site'], project, console: {log() {}}});
        await stage();
        expect(await project.readText('site/CNAME')).toBe('dist.example.test\n');
        // A publication from a checkout that lost the file restores it.
        project.storage.delete('site/CNAME');
        await stage();
        expect(await project.readText('site/CNAME')).toBe('dist.example.test\n');
    });

    it('Pages staging refuses to keep the CNAME of a former custom domain when the base URL returns to github.io', async () => {
        const project = memoryProject();
        await project.write('js/developer/tools/distribution.json', JSON.stringify({version: '0.1.0', baseUrl: 'https://owner.github.io/WebFront/'}));
        const environment = encode('environment');
        const starterBytes = encode('starter');
        const manifest = {version: '0.1.0', integrity: await integrity(environment), starters: {life: {archive: 'life-starter.tgz', integrity: await integrity(starterBytes)}}};
        await project.write('distribution/environment.tgz', environment);
        await project.write('distribution/life-starter.tgz', starterBytes);
        await project.write('distribution/provision.json', JSON.stringify(manifest));
        await project.write('distribution/DeveloperReleaseData.hpp', bootstrapHeader('0.1.0', 'https://owner.github.io/WebFront/', manifest.integrity));
        await project.write('site/CNAME', 'dist.example.test\n');
        const stage = () => stageSite({args: ['distribution', 'site'], project, console: {log() {}}});
        await rejects(stage(), /still has a CNAME file/);
        project.storage.delete('site/CNAME');
        await stage();
        expect(project.storage.has('site/CNAME')).toBe(false);
    });

    it('a custom domain is the root of the site and a github.io address has none', () => {
        expect(customDomain('https://dist.example.test')).toBe('dist.example.test');
        expect(customDomain('https://owner.github.io/WebFront/')).toBe('');
        throws(() => customDomain('https://dist.example.test/developer/'));
        throws(() => customDomain('https://dist.example.test:8443/'));
        throws(() => customDomain('http://dist.example.test/'));
    });

    it('bootstrap configuration is pinned and rejects unsafe version/base inputs', async () => {
        const digest = await integrity(encode('environment'));
        const header = bootstrapHeader('0.1.0', 'https://owner.github.io/WebFront/', digest);
        expect(header).toMatch(/https:\/\/owner.github.io\/WebFront\/developer\/0.1.0\/environment.tgz/);
        expect(header).not.toMatch(/latest/);
        throws(() => versionPath('../evil'));
        throws(() => bootstrapHeader('0.1.0', 'http://owner.test/', digest));
    });

    it('Life recipes select their generated provider and reject invalid provider names', async () => {
        for (const provider of ['LifeUI', '../escape']) {
            const bytes = starter({'package/webfront.project.json': encode(recipe({providers: {[provider]: {entry: 'main', output: `generated/${provider}.hpp`}}}))});
            let writes = 0;
            const bridge = {cppFunction: () => async () => { writes++; return 'transaction'; }};
            const config = {starters: {life: {url: '/life-starter.tgz', integrity: await integrity(bytes)}}};
            const create = () => createStarter(bridge, config, ['life', 'ui'], {fetchImpl: async () => new Response(bytes)});
            if (provider === 'LifeUI') expect(await create()).toMatch(/generated\/LifeUI.hpp/);
            else { await rejects(create(), /provider name/); expect(writes).toBe(0); }
        }
        throws(() => distributionFiles({starters: {life: {archive: '../life-starter.tgz'}}}));
    });

    describe('the origin of a pinned URL (ADR-021)', () => {
        const base = 'https://dist.webfront.fr/';
        const previous = 'https://ambroise-leclerc.github.io/WebFront/';
        const onBase = 'https://dist.webfront.fr/developer/0.6.5/environment.tgz';
        const onPrevious = version => `https://ambroise-leclerc.github.io/WebFront/developer/${version}/environment.tgz`;

        it('accepts a pin on the origin of baseUrl without a waiver', () => {
            expect(pinOrigin(onBase, base, undefined, '0.6.5')).toEqual({waiver: false});
        });

        it('refuses a pin off the origin of baseUrl when no migration is declared', () => {
            throws(() => pinOrigin(onPrevious('0.6.4'), base, undefined, '0.6.4'), /declares no previousBase/);
        });

        it('tolerates a pin before baseSince under the declared previousBase, with a warning that names the waiver', () => {
            const result = pinOrigin(onPrevious('0.6.4'), base, previous, '0.6.4', '0.6.5');
            expect(result.waiver).toBe(true);
            expect(result.warning).toMatch(/migration waiver/);
        });

        it('refuses the waiver for the first version published under the new base and for any later one', () => {
            for (const version of ['0.6.5', '0.6.6', '0.7.0', '1.0.0'])
                throws(() => pinOrigin(onPrevious(version), base, previous, version, '0.6.5'), /covers only versions before 0.6.5/);
        });

        it('orders versions numerically, not as text, and a prerelease before its release', () => {
            expect(precedes('0.6.4', '0.6.10')).toBe(true);
            expect(precedes('0.6.10', '0.6.4')).toBe(false);
            expect(precedes('0.6.5-rc.1', '0.6.5')).toBe(true);
            expect(precedes('0.6.5', '0.6.5')).toBe(false);
            expect(precedes('0.6.5', '0.6.5-rc.1')).toBe(false);
            throws(() => precedes('0.6', '0.6.5'), /Invalid version/);
        });

        it('orders prereleases by SemVer: numeric identifiers as numbers, below text, and a prefix before its extension', () => {
            // The SemVer 2.0.0 example: 1.0.0-alpha < 1.0.0-alpha.1 < 1.0.0-alpha.beta < 1.0.0-beta < 1.0.0-beta.2 < 1.0.0-beta.11 < 1.0.0-rc.1 < 1.0.0
            const ordered = ['1.0.0-alpha', '1.0.0-alpha.1', '1.0.0-alpha.beta', '1.0.0-beta', '1.0.0-beta.2', '1.0.0-beta.11', '1.0.0-rc.1', '1.0.0'];
            for (let low = 0; low < ordered.length; low++)
                for (let high = 0; high < ordered.length; high++)
                    expect(precedes(ordered[low], ordered[high])).withContext(`${ordered[low]} before ${ordered[high]}`).toBe(low < high);
            expect(precedes('0.7.0-rc.2', '0.7.0-rc.10')).toBe(true);
            expect(precedes('0.7.0-rc.10', '0.7.0-rc.2')).toBe(false);
        });

        it('does not tolerate a pin past a prerelease baseSince through the order of its identifiers', () => {
            const version = '0.7.0-rc.10';
            throws(() => pinOrigin(onPrevious(version), base, previous, version, '0.7.0-rc.2'), /covers only versions before 0.7.0-rc.2/);
            expect(pinOrigin(onPrevious('0.7.0-rc.2'), base, previous, '0.7.0-rc.2', '0.7.0-rc.10').waiver).toBe(true);
        });

        it('requires baseSince with previousBase, and previousBase with baseSince', () => {
            throws(() => pinOrigin(onPrevious('0.6.4'), base, previous, '0.6.4'), /requires baseSince/);
            throws(() => pinOrigin(onBase, base, undefined, '0.6.5', '0.6.5'), /only meaningful with previousBase/);
        });

        it('refuses a pin outside both the base and the declared previousBase', () => {
            throws(() => pinOrigin('https://elsewhere.example/developer/0.6.4/environment.tgz', base, previous, '0.6.4', '0.6.5'), /neither/);
        });

        it('refuses a stale waiver once the header pins a version published under the new base', () => {
            throws(() => pinOrigin(onBase, base, previous, '0.6.5', '0.6.5'), /Stale migration waiver/);
        });

        it('refuses a previousBase on the origin of baseUrl', () => {
            throws(() => pinOrigin(onBase, base, base, '0.6.5', '0.6.5'), /another origin/);
        });
    });
});
