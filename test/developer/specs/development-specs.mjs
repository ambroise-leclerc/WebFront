// Specs of wf dev's host side, which supplement the real-WASM browser regression in test/web/developer-specs.mjs.
import {observedRead, inputsChanged, compilerSession, waitForPoll, buildEntries, diagnosticsText, dev} from '../../../js/developer/environment/development.mjs';
import {memoryBridge} from '../memory-bridge.mjs';
import {rejects, throws} from '../support.mjs';

describe('wf dev', () => {
    const encode = text => new TextEncoder().encode(text);
    const lock = {format: 1, roots: {}, packages: {}};


    it('polling compares read-time bytes and missing probes and ignores unrelated writes', async () => {
        const {bridge, storage: files} = memoryBridge(new Map([['ui/main.ts', encode('one')]]));
        const observed = new Map(), budget = {bytes: 0};
        await observedRead(bridge, 'ui/main.ts', observed, budget);
        await rejects(observedRead(bridge, 'ui/new.ts', observed, budget), /Cannot read project file/);
        expect(await inputsChanged(bridge, observed)).toBe(false);
        files.set('unrelated.ts', encode('ignored'));
        expect(await inputsChanged(bridge, observed)).toBe(false);
        files.set('ui/main.ts', encode('two'));
        expect(await inputsChanged(bridge, observed)).toBe(true);
        await observedRead(bridge, 'ui/main.ts', observed, {bytes: 0});
        files.set('ui/new.ts', encode('created'));
        expect(await inputsChanged(bridge, observed)).toBe(true);
        await observedRead(bridge, 'ui/new.ts', observed, {bytes: 0});
        files.delete('ui/main.ts');
        expect(await inputsChanged(bridge, observed)).withContext('deletion is a change').toBe(true);
    });

    it('a polling pass bounds bytes and paths, reports non-missing failures', async () => {
        const {bridge} = memoryBridge(new Map([['main.ts', encode('file')]]));
        await rejects(observedRead(bridge, 'main.ts', new Map(), {bytes: 32 * 1024 * 1024}), /32 MiB/);
        const paths = new Map(Array.from({length: 1024}, (_, n) => [`file-${n}`, 'missing']));
        await rejects(observedRead(bridge, 'extra', paths, {bytes: 0}), /1024 input paths/);
        const broken = {cppFunction: () => async () => { throw new Error('Connection closed'); }};
        await rejects(inputsChanged(broken, new Map([['file', 'old']])), /Connection closed/);
    });

    it('Ctrl+C terminates an active compiler, rejects once, and drops late read answers', async () => {
        let callback, terminated = 0;
        const messages = [];
        const worker = {postMessage: message => messages.push(message), terminate: () => ++terminated,
            set onmessage(value) { callback = value; }};
        const controller = new AbortController();
        const session = compilerSession({cppFunction: () => () => new Promise(() => {})}, controller.signal, () => worker);
        const pending = session.rebuild({entry: 'main.ts', lock});
        callback({data: {type: 'read', id: 1, path: 'main.ts'}});
        controller.abort();
        await rejects(pending, error => error.exitCode === 130);
        callback({data: {type: 'result', files: new Map()}});
        session.close();
        expect(terminated).toBe(1);
        expect(messages.length).toBe(1);
        await rejects(session.rebuild({entry: 'main.ts', lock}), error => error.exitCode === 130);
        await rejects(waitForPoll(controller.signal), error => error.exitCode === 130);
    });

    it('a compiler session reuses its Worker and serializes rebuilds', async () => {
        let callback;
        const worker = {postMessage() {}, terminate() {}, set onmessage(value) { callback = value; }};
        const session = compilerSession({}, undefined, () => worker);
        try {
            const first = session.rebuild({entry: 'one', lock});
            await rejects(session.rebuild({entry: 'two', lock}), /already running/);
            callback({data: {type: 'result', files: new Map([['app.js', encode('one')]])}});
            expect((await first).size).toBe(1);
            const second = session.rebuild({entry: 'two', lock});
            callback({data: {type: 'error', error: 'syntax', diagnostics: [{text: 'bad', location: {file: 'project:two.ts', line: 4, column: 2}}]}});
            await rejects(second, error => diagnosticsText(error).startsWith('two.ts:4:3: bad'));
        } finally { session.close(); }
    });

    it('build selects one explicit entry or every declared entry', () => {
        expect(buildEntries({entries: {main: 'ui/main.ts'}})).toEqual([['', 'ui/main.ts']]);
        expect(buildEntries({entries: {one: 'one.ts', two: 'two.ts'}})).toEqual([['one', 'one.ts'], ['two', 'two.ts']]);
        expect(buildEntries({entries: {one: 'one.ts', two: 'two.ts'}}, 'two')).toEqual([['', 'two.ts']]);
        expect(buildEntries({entries: {'app:one': 'one.ts', 'app..two': 'two.ts', CON: 'three.ts'}})).toEqual([['app%3Aone', 'one.ts'], ['app%2E%2Etwo', 'two.ts'], ['%43ON', 'three.ts']]);
        throws(() => buildEntries(null), /Usage: wf build/);
    });


    it('Ctrl+C during commit still displays the committed preview before stopping dev', async () => {
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
            await rejects(dev([], {bridge, signal: controller.signal, preview: frame}), error => error.exitCode === 130);
            expect(project.storage.get('.webfront/developer/preview/app.js')).toEqual(encode('console.log("committed")'));
            expect(navigations.length).withContext('a completed publication must be displayed even when Ctrl+C arrived during commit').toBe(1);
            expect(navigations[0]).toMatch(/^\/webfront\/developer\/preview\/index.html\?build=/);
            expect(frame.hidden).toBe(false);
            expect(project.pending()).toBe(0);
            expect(terminated).toBe(1);
        } finally {
            if (previousWorker === undefined) delete globalThis.Worker;
            else globalThis.Worker = previousWorker;
        }
    });
});
