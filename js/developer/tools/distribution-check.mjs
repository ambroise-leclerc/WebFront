import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import {tar} from './archive.mjs';
import {stageSite, bootstrapHeader, versionPath, distributionFiles} from './site.mjs';
import {compress} from '../environment/embed.mjs';
import {createStarter} from '../environment/starter.mjs';
const encode = value => new TextEncoder().encode(typeof value === 'string' ? value : JSON.stringify(value));
const integrity = bytes => 'sha512-' + createHash('sha512').update(bytes).digest('base64');

const recipe = (fields = {}) => ({format: 1, name: 'pi-starter', version: '0.1.0', dependencies: {react: '19.3.1'},
    entries: {main: 'main.tsx'}, ...fields});

function starter(overrides = {}) {
    return compress(tar(new Map(Object.entries({
        'package/main.tsx': encode('export default function Pi() {}'),
        'package/webfront.project.json': encode(recipe()),
        ...overrides,
    }))));
}

test('starter integrity, recipes and archive paths are checked before any project writes', async () => {
    globalThis.location = new URL('http://localhost:9002/');
    const bytes = starter();
    const calls = [];
    const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return 'transaction'; }};
    const config = {starters: {pi: {url: 'https://pages.test/developer/0.1.0/pi-starter.tgz', integrity: integrity(bytes)}}};
    const fetchImpl = async () => new Response(bytes);
    const help = await createStarter(bridge, config, ['pi', 'ui'], {fetchImpl});
    assert.match(help, /wf init\nwf install react@19.3.1/);
    assert.match(help, /wf preview ui\/main.tsx/);
    assert.equal(calls[0][0], 'webfront.developer.beginNew');
    assert.equal(calls.at(-1)[0], 'webfront.developer.commit');
    calls.length = 0;
    await assert.rejects(createStarter(bridge, {starters: {pi: {...config.starters.pi, integrity: integrity(encode('bad'))}}}, ['pi', 'ui'], {fetchImpl}), /Integrity check/);
    assert.equal(calls.length, 0);
    for (const bytes of [starter({'package/../escape': encode('bad')}), starter({'package/webfront.project.json': encode(recipe({dependencies: {react: 'latest'}}))})]) {
        await assert.rejects(createStarter(bridge, {starters: {pi: {...config.starters.pi, integrity: integrity(bytes)}}}, ['pi', 'ui'], {fetchImpl: async () => new Response(bytes)}));
        assert.equal(calls.length, 0);
    }
    delete globalThis.location;
});

test('starter replacement refusal aborts the publication rather than overwriting files', async () => {
    globalThis.location = new URL('http://localhost:9002/');
    const bytes = starter();
    const calls = [];
    const bridge = {cppFunction: name => async () => {
        calls.push(name);
        if (name.endsWith('.commit')) throw new Error('Starter destination already exists');
        return 'transaction';
    }};
    await assert.rejects(createStarter(bridge, {starters: {pi: {url: '/starter.tgz', integrity: integrity(bytes)}}}, ['pi', 'ui'], {fetchImpl: async () => new Response(bytes)}), /already exists/);
    assert.equal(calls.at(-1), 'webfront.developer.abort');
    delete globalThis.location;
});

test('Pages staging retains older versions and refuses different bytes under an existing version', async () => {
    const root = await mkdtemp(join(tmpdir(), 'webfront-distribution-'));
    try {
        const distribution = join(root, 'distribution');
        const site = join(root, 'site');
        await mkdir(distribution);
        const environment = encode('environment');
        const pi = encode('starter');
        const write = async version => {
            const manifest = {version, integrity: integrity(environment), starters: {pi: {archive: 'pi-starter.tgz', integrity: integrity(pi)}, ...(version !== '0.1.0' ? {life: {archive: 'life-starter.tgz', integrity: integrity(pi)}} : {})}};
            await writeFile(join(distribution, 'environment.tgz'), environment);
            await writeFile(join(distribution, 'pi-starter.tgz'), pi);
            if (version !== '0.1.0') await writeFile(join(distribution, 'life-starter.tgz'), pi);
            await writeFile(join(distribution, 'provision.json'), JSON.stringify(manifest));
            await writeFile(join(distribution, 'DeveloperReleaseData.hpp'), bootstrapHeader(version, 'https://owner.github.io/WebFront/', manifest.integrity));
        };
        await write('0.1.0');
        await stageSite(distribution, site);
        await stageSite(distribution, site);
        await write('0.2.0');
        await stageSite(distribution, site);
        assert.equal(await readFile(join(site, 'developer/0.1.0/environment.tgz'), 'utf8'), 'environment');
        assert.match(await readFile(join(site, 'index.html'), 'utf8'), /developer\/0.2.0\/life-starter.tgz/);
        assert.equal(await readFile(join(site, 'developer/0.2.0/life-starter.tgz'), 'utf8'), 'starter');
        await write('0.3.0');
        await writeFile(join(distribution, 'life-starter.tgz'), 'packed-starter');
        const manifest = JSON.parse(await readFile(join(distribution, 'provision.json')));
        manifest.starters.life.integrity = integrity(encode('packed-starter'));
        await writeFile(join(distribution, 'provision.json'), JSON.stringify(manifest));
        await stageSite(distribution, site);
        assert.equal(await readFile(join(site, 'developer/0.1.0/environment.tgz'), 'utf8'), 'environment');
        assert.equal(await readFile(join(site, 'developer/0.2.0/life-starter.tgz'), 'utf8'), 'starter');
        assert.equal(await readFile(join(site, 'developer/0.3.0/life-starter.tgz'), 'utf8'), 'packed-starter');
        await writeFile(join(distribution, 'DeveloperReleaseData.hpp'), 'changed');
        await assert.rejects(stageSite(distribution, site), /increment the distribution version/);
        assert.match(await readFile(join(site, 'developer/0.2.0/DeveloperReleaseData.hpp'), 'utf8'), /namespace webfront/);
        await writeFile(join(distribution, 'environment.tgz'), 'corrupt');
        await assert.rejects(stageSite(distribution, site), /checksum mismatch/);
    } finally { await rm(root, {recursive: true, force: true}); }
});

test('bootstrap configuration is pinned and rejects unsafe version/base inputs', () => {
    const header = bootstrapHeader('0.1.0', 'https://owner.github.io/WebFront/', integrity(encode('environment')));
    assert.match(header, /https:\/\/owner.github.io\/WebFront\/developer\/0.1.0\/environment.tgz/);
    assert.doesNotMatch(header, /latest/);
    assert.throws(() => versionPath('../evil'));
    assert.throws(() => bootstrapHeader('0.1.0', 'http://owner.test/', integrity(encode('environment'))));
});

test('Life recipes select their generated provider and reject invalid provider names', async () => {
    globalThis.location = new URL('http://localhost:9002/');
    try {
        for (const provider of ['LifeUI', '../escape']) {
            const bytes = starter({'package/webfront.project.json': encode(recipe({providers: {[provider]: {entry: 'main', output: `generated/${provider}.hpp`}}}))});
            let writes = 0;
            const bridge = {cppFunction: () => async () => { writes++; return 'transaction'; }};
            const config = {starters: {life: {url: '/life-starter.tgz', integrity: integrity(bytes)}}};
            const create = () => createStarter(bridge, config, ['life', 'ui'], {fetchImpl: async () => new Response(bytes)});
            if (provider === 'LifeUI') assert.match(await create(), /generated\/LifeUI.hpp/);
            else { await assert.rejects(create(), /provider name/); assert.equal(writes, 0); }
        }
        assert.throws(() => distributionFiles({starters: {life: {archive: '../life-starter.tgz'}}}));
    } finally { delete globalThis.location; }
});
