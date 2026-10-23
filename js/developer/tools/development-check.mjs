// Host tests supplement the real-WASM browser regression in developer-specs.mjs.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {build as bundle} from '../vendor/node_modules/esbuild/lib/main.js';
import * as esbuild from '../vendor/node_modules/esbuild/lib/main.js';
import {bundleContext} from '../environment/bundler.mjs';
import {memoryBridge} from './memory-bridge.mjs';

// Resolve the browser module using the same locked dependencies as the environment archive.
const compiled = await bundle({entryPoints: ['js/developer/environment/development.mjs'], bundle: true, write: false,
    format: 'esm', platform: 'browser', nodePaths: ['js/developer/vendor/node_modules']});
const temporary = await mkdtemp(join(tmpdir(), 'webfront-development-test-'));
let api;
try {
    const path = join(temporary, 'development.mjs');
    await writeFile(path, compiled.outputFiles[0].contents);
    api = await import(pathToFileURL(path));
} finally { await rm(temporary, {recursive: true, force: true}); }
const {observedRead, inputsChanged, compilerSession, waitForPoll, buildEntries, diagnosticsText, dev} = api;
const encode = text => new TextEncoder().encode(text);

const lock = {format: 1, roots: {}, packages: {}};
test('one esbuild context reads changed imports, reports syntax errors and recovers', async () => {
    const files = new Map([['main.ts', encode("import {value} from './value'; console.log(value)")], ['value.ts', encode('export const value = 1')]]);
    const context = await bundleContext(esbuild, {entry: 'main.ts', lock, readProject: async path => {
        if (!files.has(path)) throw new Error('missing');
        return files.get(path);
    }});
    try {
        const text = async () => new TextDecoder().decode((await context.rebuild()).get('app.js'));
        assert.match(await text(), /console.log\(1\)/);
        files.set('value.ts', encode('export const value = 2'));
        assert.match(await text(), /console.log\(2\)/);
        files.set('value.ts', encode('export const value = ;'));
        await assert.rejects(text(), error => diagnosticsText(error).includes('value.ts:1:'));
        files.set('value.ts', encode('export const value = 3'));
        assert.match(await text(), /console.log\(3\)/);
    } finally { await context.dispose(); }
});

test('polling compares read-time bytes and missing probes and ignores unrelated writes', async () => {
    const {bridge, storage: files} = memoryBridge(new Map([['ui/main.ts', encode('one')]]));
    const observed = new Map(), budget = {bytes: 0};
    await observedRead(bridge, 'ui/main.ts', observed, budget);
    await assert.rejects(observedRead(bridge, 'ui/new.ts', observed, budget), /Cannot read project file/);
    assert.equal(await inputsChanged(bridge, observed), false);
    files.set('unrelated.ts', encode('ignored'));
    assert.equal(await inputsChanged(bridge, observed), false);
    files.set('ui/main.ts', encode('two'));
    assert.equal(await inputsChanged(bridge, observed), true);
    await observedRead(bridge, 'ui/main.ts', observed, {bytes: 0});
    files.set('ui/new.ts', encode('created'));
    assert.equal(await inputsChanged(bridge, observed), true);
    await observedRead(bridge, 'ui/new.ts', observed, {bytes: 0});
    files.delete('ui/main.ts');
    assert.equal(await inputsChanged(bridge, observed), true, 'deletion is a change');
});

test('a polling pass bounds bytes and paths, reports non-missing failures', async () => {
    const {bridge} = memoryBridge(new Map([['main.ts', encode('file')]]));
    await assert.rejects(observedRead(bridge, 'main.ts', new Map(), {bytes: 32 * 1024 * 1024}), /32 MiB/);
    const paths = new Map(Array.from({length: 1024}, (_, n) => [`file-${n}`, 'missing']));
    await assert.rejects(observedRead(bridge, 'extra', paths, {bytes: 0}), /1024 input paths/);
    const broken = {cppFunction: () => async () => { throw new Error('Connection closed'); }};
    await assert.rejects(inputsChanged(broken, new Map([['file', 'old']])), /Connection closed/);
});

test('Ctrl+C terminates an active compiler, rejects once, and drops late read answers', async () => {
    let callback, terminated = 0;
    const messages = [];
    const worker = {postMessage: message => messages.push(message), terminate: () => ++terminated,
        set onmessage(value) { callback = value; }};
    const controller = new AbortController();
    const session = compilerSession({cppFunction: () => () => new Promise(() => {})}, controller.signal, () => worker);
    const pending = session.rebuild({entry: 'main.ts', lock});
    callback({data: {type: 'read', id: 1, path: 'main.ts'}});
    controller.abort();
    await assert.rejects(pending, error => error.exitCode === 130);
    callback({data: {type: 'result', files: new Map()}});
    session.close();
    assert.equal(terminated, 1);
    assert.equal(messages.length, 1);
    await assert.rejects(session.rebuild({entry: 'main.ts', lock}), error => error.exitCode === 130);
    await assert.rejects(waitForPoll(controller.signal), error => error.exitCode === 130);
});

test('a compiler session reuses its Worker and serializes rebuilds', async () => {
    let callback;
    const worker = {postMessage() {}, terminate() {}, set onmessage(value) { callback = value; }};
    const session = compilerSession({}, undefined, () => worker);
    try {
        const first = session.rebuild({entry: 'one', lock});
        await assert.rejects(session.rebuild({entry: 'two', lock}), /already running/);
        callback({data: {type: 'result', files: new Map([['app.js', encode('one')]])}});
        assert.equal((await first).size, 1);
        const second = session.rebuild({entry: 'two', lock});
        callback({data: {type: 'error', error: 'syntax', diagnostics: [{text: 'bad', location: {file: 'project:two.ts', line: 4, column: 2}}]}});
        await assert.rejects(second, error => diagnosticsText(error).startsWith('two.ts:4:3: bad'));
    } finally { session.close(); }
});

test('build selects one explicit entry or every declared entry', () => {
    assert.deepEqual(buildEntries({entries: {main: 'ui/main.ts'}}), [['', 'ui/main.ts']]);
    assert.deepEqual(buildEntries({entries: {one: 'one.ts', two: 'two.ts'}}), [['one', 'one.ts'], ['two', 'two.ts']]);
    assert.deepEqual(buildEntries({entries: {one: 'one.ts', two: 'two.ts'}}, 'two'), [['', 'two.ts']]);
    assert.deepEqual(buildEntries({entries: {'app:one': 'one.ts', 'app..two': 'two.ts', CON: 'three.ts'}}),
        [['app%3Aone', 'one.ts'], ['app%2E%2Etwo', 'two.ts'], ['%43ON', 'three.ts']]);
    assert.throws(() => buildEntries(null), /Usage: wf build/);
});


test('Ctrl+C during commit still displays the committed preview before stopping dev', async () => {
    const project = memoryBridge(new Map([['webfront.project.json', encode(JSON.stringify({
        format: 1, name: 'commit-race', version: '0.1.0', entries: {main: 'main.ts'},
    }))]]));
    const controller = new AbortController();
    const bridge = {cppFunction: name => async (...args) => {
        const result = await project.bridge.cppFunction(name)(...args);
        // The native commit has completed, but its acknowledgement has not reached dev yet.
        if (name === 'webfront.developer.commit') controller.abort();
        return result;
    }};
    let terminated = 0;
    const previousWorker = globalThis.Worker;
    globalThis.Worker = class {
        postMessage(message) {
            if (message.type === 'build') queueMicrotask(() => this.onmessage({data: {
                type: 'result', files: new Map([['app.js', encode('console.log("committed")')]]),
            }}));
        }
        terminate() { ++terminated; }
    };
    const navigations = [];
    const frame = {hidden: true, contentDocument: null, contentWindow: {location: {replace: url => navigations.push(url)}},
        addEventListener() {}, removeEventListener() {}};
    try {
        await assert.rejects(dev([], {bridge, signal: controller.signal, preview: frame}), error => error.exitCode === 130);
        assert.deepEqual(project.storage.get('.webfront/developer/preview/app.js'), encode('console.log("committed")'));
        assert.equal(navigations.length, 1, 'a completed publication must be displayed even when Ctrl+C arrived during commit');
        assert.match(navigations[0], /^\/webfront\/developer\/preview\/index.html\?build=/);
        assert.equal(frame.hidden, false);
        assert.equal(project.pending(), 0);
        assert.equal(terminated, 1);
    } finally {
        if (previousWorker === undefined) delete globalThis.Worker;
        else globalThis.Worker = previousWorker;
    }
});
