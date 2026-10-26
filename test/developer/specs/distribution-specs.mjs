// Specs of the developer distribution: the starters, the Pages staging and the pinned bootstrap header.
import {tar} from '../../../js/developer/tools/archive.mjs';
import {bootstrapHeader, versionPath, distributionFiles} from '../../../js/developer/tools/distribution.mjs';
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
});
