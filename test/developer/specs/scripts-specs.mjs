// Specs of wf run and wf exec: permissions, command resolution, the page side of the context API and the Worker's
// termination, with a scripted stand-in for the Worker and a fresh evaluation of the real Worker module.
import {checkRead, checkWrite, checkFetch, listable, scriptCommand, parseRunOptions, execTarget, lockedPackage, scriptHost,
    runWorker, runResult, runDeadline, secondsLeft, ScriptExit, nodeBuiltin, nodeImport, createFinder, validateManifest}
    from '../../../js/developer/environment/packages.bundle.mjs';
import {memoryBridge} from '../memory-bridge.mjs';
import {rejects, throws, freshModule} from '../support.mjs';

describe('wf run and wf exec', () => {
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    // The terminal's tokenizer splits on spaces outside quotes; terminal.mjs itself needs xterm.
    const tokenize = text => text.match(/"[^"]*"|\S+/g).map(word => word.replace(/^"|"$/g, ''));
    const permissions = {files: ['ui', 'data/in.json'], publish: ['generated', '.webfront/x', 'webfront.project.json'], fetch: ['https://api.test']};

    it('context operations are checked against the manifest permissions', () => {
        expect(checkRead(permissions, 'ui/main.tsx')).toBe('ui/main.tsx');
        expect(checkRead(permissions, 'generated/out.txt')).withContext('a writable path is readable').toBe('generated/out.txt');
        expect(checkRead(permissions, 'data/in.json')).toBe('data/in.json');
        for (const path of ['data/other.json', 'uix/a', 'webfront-packages.lock.json'])
            throws(() => checkRead(permissions, path), /Permission denied: reading .* permissions\.files/);
        throws(() => checkRead(permissions, '../outside'), /Not a project-relative path/);
        throws(() => checkRead(permissions, ''), /Not a project-relative path/);
        expect(checkWrite(permissions, 'generated/a/b.txt')).toBe('generated/a/b.txt');
        throws(() => checkWrite(permissions, 'ui/main.tsx'), /Permission denied: writing ui\/main.tsx .* permissions\.publish/);
        for (const path of ['webfront.project.json', '.webfront/x/y'])
            throws(() => checkWrite(permissions, path), /managed by wf and never written by a script/);
        expect(checkFetch(permissions, 'https://api.test/v1?q=1').href).toBe('https://api.test/v1?q=1');
        throws(() => checkFetch(permissions, 'https://other.test/'), /Permission denied: fetching https:\/\/other.test/);
        throws(() => checkFetch({}, '/local', 'http://localhost:9002/'), /permissions\.fetch/);
    });

    it('wf run resolves a script of the manifest to a module or a wf command, without a shell', () => {
        const manifest = validateManifest({format: 1, name: 'app', version: '1.0.0',
            scripts: {gen: 'tools/gen.ts --fast', build: 'wf embed main', loop: 'wf run loop'}});
        expect(scriptCommand(manifest, 'gen', ['a b'], tokenize)).toEqual({kind: 'module', entry: {namespace: 'project', path: 'tools/gen.ts'}, args: ['--fast', 'a b']});
        expect(scriptCommand(manifest, 'build', ['--x'], tokenize)).toEqual({kind: 'wf', argv: ['embed', 'main', '--x']});
        throws(() => scriptCommand(manifest, 'missing', [], tokenize), /No script "missing" .* \(scripts: build, gen, loop\)/);
        throws(() => scriptCommand(manifest, 'loop', [], tokenize, 4), /more than 4 levels/);
        throws(() => scriptCommand({...manifest, scripts: {bad: '../x.ts'}}, 'bad', [], tokenize), /Unsafe package path/);
        expect(parseRunOptions(['--timeout', '5', 'gen', '--timeout', '9'], 'u')).toEqual({timeout: 5, target: 'gen', args: ['--timeout', '9']});
        expect(parseRunOptions(['gen'], 'u').timeout).toBe(600);
        for (const args of [['--timeout', '0', 'gen'], ['--timeout', '3601', 'gen'], ['--timeout', '1.5', 'gen']])
            throws(() => parseRunOptions(args, 'u'), /--timeout expects whole seconds from 1 to 3600/);
        throws(() => parseRunOptions([], 'wf run <script>'), /Usage: wf run <script>/);
        throws(() => parseRunOptions(['--other', 'gen'], 'wf run <script>'), /Usage/);
    });

    const node = (name, version, manifest, files = ['package.json', 'bin/tool.mjs']) =>
        ({name, version, manifest: {name, version, ...manifest}, files, dependencies: {}});
    const lockOf = (...nodes) => ({format: 1,
        roots: Object.fromEntries(nodes.map(n => [n.name, {requested: n.version, id: `${n.name.replace('/', '+')}@${n.version}`}])),
        packages: Object.fromEntries(nodes.map(n => [`${n.name.replace('/', '+')}@${n.version}`, n]))});

    it('wf exec runs only a webfront.bin entry of a locked package', () => {
        const lock = lockOf(
            node('tool', '1.0.0', {webfront: {bin: './bin/tool.mjs'}}),
            node('@scope/multi', '2.0.0', {webfront: {bin: {one: 'bin/tool.mjs', two: 'bin/tool.mjs'}}}),
            node('nodeonly', '1.0.0', {bin: {nodeonly: 'cli.js'}}),
            node('browser', '1.0.0', {main: 'index.js'}),
            node('broken', '1.0.0', {webfront: {bin: 'bin/missing.mjs'}}));
        expect(execTarget(lock, 'tool')).toEqual({id: 'tool@1.0.0', name: 'tool', entry: {namespace: 'npm', path: 'tool@1.0.0/bin/tool.mjs'}});
        expect(execTarget(lock, '@scope/multi:two').entry.path).toBe('@scope+multi@2.0.0/bin/tool.mjs');
        throws(() => execTarget(lock, '@scope/multi'), /has the WebFront entries one, two: run wf exec @scope\/multi:<entry>/);
        throws(() => execTarget(lock, 'tool:other'), /has the WebFront entries tool/);
        throws(() => execTarget(lock, 'nodeonly'), /only declares an npm "bin" \(a Node\.js program\)/);
        throws(() => execTarget(lock, 'browser'), /has no WebFront executable entry/);
        throws(() => execTarget(lock, 'broken'), /names a missing file/);
        throws(() => execTarget(lock, 'absent'), /not installed: run wf install absent/);
        throws(() => execTarget(null, 'tool'), /not installed/);
    });

    it('Node.js built-in modules are recognized, with and without the node: prefix', () => {
        for (const [specifier, name] of [['fs', 'fs'], ['node:fs', 'fs'], ['fs/promises', 'fs'], ['child_process', 'child_process'], ['node:test', 'test']])
            expect(nodeBuiltin(specifier)).toBe(name);
        for (const specifier of ['react', 'react-dom/client', './fs', 'fsevents']) expect(nodeBuiltin(specifier)).toBe(null);
    });

    const project = () => memoryBridge(new Map([
        ['ui/main.tsx', encoder.encode('main')],
        ['ui/parts/a.ts', encoder.encode('a')],
        ['generated/old.txt', encoder.encode('old')],
        ['secret/key', encoder.encode('k')],
    ]));

    it('the page answers context calls within the permissions and publishes each write', async () => {
        const memory = project();
        const host = scriptHost({bridge: memory.bridge, permissions, lock: lockOf(node('tool', '1.0.0', {}))});
        expect(decoder.decode(await host.handle({operation: 'read', args: ['ui/main.tsx']}))).toBe('main');
        expect(await host.handle({operation: 'list', args: ['ui']})).toEqual(['main.tsx', 'parts/']);
        await rejects(host.handle({operation: 'read', args: ['secret/key']}), /Permission denied/);
        await rejects(host.handle({operation: 'list', args: ['secret']}), /Permission denied/);
        await host.handle({operation: 'write', args: ['generated/new.txt', encoder.encode('new')]});
        expect(decoder.decode(memory.storage.get('generated/new.txt'))).toBe('new');
        await rejects(host.handle({operation: 'write', args: ['ui/main.tsx', encoder.encode('x')]}), /Permission denied/);
        await rejects(host.handle({operation: 'write', args: ['generated/x', 'text']}), /expects a string or a Uint8Array/);
        expect(await host.handle({operation: 'resolve', args: ['tool']})).toEqual({name: 'tool', version: '1.0.0', integrity: undefined});
        expect(await host.handle({operation: 'resolve', args: ['absent']})).toBe(null);
        await rejects(host.handle({operation: 'shell', args: []}), /Unknown context operation/);
        expect(host.publications.map(({path, state}) => [path, state])).toEqual([['generated/new.txt', 'applied']]);
        await host.stop();
        await rejects(host.handle({operation: 'read', args: ['ui/main.tsx']}), /stopped/);
        expect(memory.pending()).toBe(0);
    });

    // A bridge that runs `action` once, when the named operation is first called.
    function hooked(bridge, operation, action) {
        let fired = false;
        return {cppFunction: name => async (...args) => {
            if (!fired && name.endsWith('.' + operation) && (operation !== 'write' || args[2] > 0)) { fired = true; await action(); }
            return bridge.cppFunction(name)(...args);
        }};
    }

    it('a stop while a write is staging abandons it: the file is unchanged and no transaction remains', async () => {
        const memory = project();
        let host;
        const bridge = hooked(memory.bridge, 'write', () => { host.stop(); });
        host = scriptHost({bridge, permissions, lock: null});
        const large = new Uint8Array(3 * 256 * 1024).fill(7);
        await rejects(host.handle({operation: 'write', args: ['generated/old.txt', large]}), /stopped/);
        await host.stop();
        expect(decoder.decode(memory.storage.get('generated/old.txt'))).toBe('old');
        expect(memory.pending()).withContext('the staged transaction is aborted').toBe(0);
        expect(host.publications.map(({state}) => state)).toEqual(['abandoned']);
    });

    it('a stop after the commit was sent lets the publication complete with the whole new content', async () => {
        const memory = project();
        let host;
        let stopped;
        const bridge = hooked(memory.bridge, 'commit', () => { stopped = host.stop(); });
        host = scriptHost({bridge, permissions, lock: null});
        const large = new Uint8Array(3 * 256 * 1024).fill(9);
        await host.handle({operation: 'write', args: ['generated/old.txt', large]});
        await stopped;
        expect(memory.storage.get('generated/old.txt')).toEqual(large);
        expect(host.publications.map(({state}) => state)).toEqual(['applied']);
        expect(memory.pending()).toBe(0);
    });

    // A stand-in for the script Worker: `behave(message, worker)` plays the script's side.
    function fakeWorker(behave) {
        const worker = {terminated: false, received: [], onmessage: null, onerror: null,
            postMessage(message) {
                this.received.push(message);
                queueMicrotask(() => behave(message, worker));
            },
            emit(data) { if (!this.terminated) this.onmessage?.({data}); },
            terminate() { this.terminated = true; }};
        return worker;
    }

    const fastRun = (behave, options = {}) => {
        const worker = fakeWorker(behave);
        const output = [];
        const host = options.host ?? {handle: async ({operation, args}) => `${operation}:${args.join(',')}`};
        const outcome = runWorker({createWorker: () => worker, host, message: {code: '', args: []}, grace: 20, timeout: 5,
            write: text => output.push(text), ...options});
        return {worker, output, outcome};
    };

    it('the exit code, console output and context replies of a run', async () => {
        const {worker, output, outcome} = fastRun((message, w) => {
            if (message.type === 'run') {
                w.emit({type: 'console', stream: 'out', text: 'line 1\nline 2'});
                w.emit({type: 'call', id: 1, operation: 'read', args: ['ui/a']});
            }
            if (message.type === 'reply') w.emit({type: 'exit', code: 3});
        });
        expect(await outcome).toEqual({reason: 'exit', code: 3});
        expect(output).toEqual(['line 1\r\nline 2\r\n']);
        expect(worker.received.find(message => message.type === 'reply')).toEqual({type: 'reply', id: 1, value: 'read:ui/a'});
        expect(worker.terminated).toBeTruthy();
        throws(() => runResult('Script gen', {reason: 'exit', code: 3}, []), error => error instanceof ScriptExit && error.exitCode === 3 && /exited with code 3/.test(error.message));
        expect(runResult('Script gen', {reason: 'exit', code: 0}, [])).toBe('Script gen completed.');
        const failed = await fastRun((message, w) => { if (message.type === 'run') w.emit({type: 'error', error: 'Error: boom'}); }).outcome;
        expect(failed).toEqual({reason: 'exit', code: 1, error: 'Error: boom'});
    });

    it('Ctrl+C aborts the script cooperatively, then terminates one that does not stop', async () => {
        const controller = new AbortController();
        const cooperative = fastRun((message, w) => { if (message.type === 'abort') w.emit({type: 'error', error: 'Error: Interrupted'}); }, {signal: controller.signal});
        controller.abort();
        expect(await cooperative.outcome).toEqual({reason: 'interrupted', code: 130, error: 'Error: Interrupted'});
        const busy = new AbortController();
        const looping = fastRun(() => {}, {signal: busy.signal});
        setTimeout(() => busy.abort(), 5);
        const outcome = await looping.outcome;
        expect(outcome).toEqual({reason: 'interrupted', code: 130, forced: 20});
        expect(looping.worker.terminated).toBeTruthy();
        expect(looping.worker.received.some(message => message.type === 'abort')).withContext('the signal is emitted before the termination').toBeTruthy();
        const publications = [{path: 'generated/a', state: 'applied'}, {path: 'generated/b', state: 'abandoned'}];
        throws(() => runResult('Script loop', outcome, publications), error => error.exitCode === 130
            && /stopped by Ctrl\+C \(terminated after the 0.02 s grace period\)/.test(error.message)
            && /Applied before the stop: generated\/a/.test(error.message) && /Abandoned, unchanged: generated\/b/.test(error.message));
    });

    it('the time limit stops a script the same way, without Ctrl+C', async () => {
        const {worker, outcome} = fastRun(() => {}, {timeout: 0.01});
        expect(await outcome).toEqual({reason: 'timeout', code: 124, forced: 20});
        expect(worker.terminated && worker.received.some(message => message.type === 'abort')).toBeTruthy();
        throws(() => runResult('Script loop', {reason: 'timeout', code: 124, forced: 20}, []), /stopped at its time limit/);
    });

    it('a reply that arrives after the end of the run is not sent to the terminated Worker', async () => {
        let release;
        const host = {handle: () => new Promise(resolve => { release = resolve; })};
        const {worker, outcome} = fastRun((message, w) => {
            if (message.type === 'run') { w.emit({type: 'call', id: 1, operation: 'read', args: []}); w.emit({type: 'exit', code: 0}); }
        }, {host});
        await outcome;
        release('late');
        await new Promise(resolve => setTimeout(resolve, 5));
        expect(worker.received.filter(message => message.type === 'reply').length).toBe(0);
    });

    it('a refused commit is aborted too, so no transaction or staging is left behind', async () => {
        const memory = project();
        const bridge = {cppFunction: name => name.endsWith('.commit')
            ? async () => { throw new Error('Cannot replace the destination'); }
            : memory.bridge.cppFunction(name)};
        const host = scriptHost({bridge, permissions, lock: null});
        await rejects(host.handle({operation: 'write', args: ['generated/old.txt', encoder.encode('new')]}), /Cannot replace/);
        await host.stop();
        expect(memory.pending()).toBe(0);
        expect(host.publications.map(({state}) => state)).toEqual(['failed']);
        expect(decoder.decode(memory.storage.get('generated/old.txt'))).toBe('old');
    });

    // Loads a fresh instance of the real script Worker module, with `self` standing in for the Worker's scope. The module
    // replaces the console methods of the page that hosts it (so that libraries log to the terminal): they are put back.
    async function scriptWorker(code, granted = {}) {
        const messages = [];
        const worker = {postMessage: message => messages.push(message)};
        const scope = Object.getOwnPropertyDescriptor(globalThis, 'self');
        const console = {...globalThis.console};
        Object.defineProperty(globalThis, 'self', {value: worker, configurable: true, writable: true});
        try {
            await import(await freshModule('js/developer/environment/script-worker.mjs'));
            worker.onmessage({data: {type: 'run', code, args: [], permissions: granted, base: 'http://localhost/'}});
            await new Promise(resolve => setTimeout(resolve, 60));
            return {messages, worker};
        } finally {
            Object.defineProperty(globalThis, 'self', scope);
            Object.assign(globalThis.console, console);
        }
    }

    it('context.exit stops the script at once, even when it is not awaited', async () => {
        const {messages} = await scriptWorker(`export default async context => {
            context.exit(3);
            context.console.log('ran after exit');
        };`);
        expect(messages).toEqual([{type: 'exit', code: 3}]);
        const caught = await scriptWorker(`export default async context => {
            try { context.exit(4); } catch {}
            context.console.log('after a caught exit');
            await context.project.write('generated/x', 'x').catch(error => context.console.log(error.message));
        };`);
        expect(caught.messages).withContext('nothing reaches the page after the first exit').toEqual([{type: 'exit', code: 4}]);
    });

    it('a write the script did not await completes after a normal end, and its failure is reported', async () => {
        const memory = project();
        const host = scriptHost({bridge: memory.bridge, permissions, lock: null});
        const unawaited = host.handle({operation: 'write', args: ['generated/later.txt', encoder.encode('later')]});
        await host.finish();
        await unawaited;
        expect(decoder.decode(memory.storage.get('generated/later.txt'))).toBe('later');
        expect(runResult('Script gen', {reason: 'exit', code: 0}, host.publications)).toBe('Script gen completed.');
        const failing = scriptHost({bridge: {cppFunction: name => name.endsWith('.commit')
            ? async () => { throw new Error('Disk full'); } : memory.bridge.cppFunction(name)}, permissions, lock: null});
        const lost = failing.handle({operation: 'write', args: ['generated/lost.txt', encoder.encode('x')]}).catch(() => {});
        await failing.finish();
        await lost;
        throws(() => runResult('Script gen', {reason: 'exit', code: 0}, failing.publications),
            error => error.exitCode === 1 && /ended without applying all its writes/.test(error.message) && /Failed, unchanged: generated\/lost.txt/.test(error.message));
        // A failure the script awaited is its own to handle: the run still completes.
        const handled = scriptHost({bridge: {cppFunction: name => name.endsWith('.commit')
            ? async () => { throw new Error('Disk full'); } : memory.bridge.cppFunction(name)}, permissions, lock: null});
        await rejects(handled.handle({operation: 'write', args: ['generated/x.txt', encoder.encode('x')]}), /Disk full/);
        await handled.finish();
        expect(runResult('Script gen', {reason: 'exit', code: 0}, handled.publications)).toBe('Script gen completed.');
        expect(memory.pending()).toBe(0);
    });

    it('the project root and the directories leading to granted paths are listable, filtered', async () => {
        const granted = {files: ['ui/parts'], publish: ['generated']};
        expect(listable(granted, 'ui/parts').keep('anything')).toBe(true);
        const root = listable(granted, '');
        expect(['ui/', 'generated/', 'secret/', 'README.md'].filter(root.keep)).toEqual(['ui/', 'generated/']);
        expect(['parts/', 'main.tsx'].filter(listable(granted, 'ui').keep)).toEqual(['parts/']);
        expect(listable(granted, '.').path).toBe('');
        throws(() => listable(granted, 'secret'), /Permission denied: listing secret/);
        throws(() => listable({}, ''), /Permission denied: listing the project root/);
        const memory = project();
        const host = scriptHost({bridge: memory.bridge, permissions: granted, lock: null});
        expect(await host.handle({operation: 'list', args: ['']})).toEqual(['generated/', 'ui/']);
        expect(await host.handle({operation: 'list', args: []})).withContext('list() lists the root').toEqual(['generated/', 'ui/']);
        expect(await host.handle({operation: 'list', args: ['ui']})).toEqual(['parts/']);
    });

    it('a time limit also bounds the scripts run through wf commands', () => {
        const outer = runDeadline(5, undefined, 1000);
        expect(outer).toBe(6000);
        expect(runDeadline(600, outer, 2000)).withContext('a nested run keeps the earlier deadline').toBe(6000);
        expect(runDeadline(1, outer, 2000)).withContext('and may set an earlier one').toBe(3000);
        expect(secondsLeft(6000, 2000)).toBe(4);
        expect(secondsLeft(6000, 7000)).toBe(0);
    });

    it('a locked name is looked up as an own key, and resolve() designates the package wf exec uses', async () => {
        const lock = lockOf(node('tool', '2.0.0', {webfront: {bin: 'bin/tool.mjs'}}));
        lock.packages['tool@1.0.0'] = node('tool', '1.0.0', {});
        throws(() => execTarget(lock, 'constructor'), /constructor is not installed/);
        expect(lockedPackage(lock, 'toString')).toBe(null);
        expect(lockedPackage(lock, 'tool')[0]).toBe('tool@2.0.0');
        const host = scriptHost({bridge: project().bridge, permissions, lock});
        expect((await host.handle({operation: 'resolve', args: ['tool']})).version).withContext('the root, not a transitive version').toBe('2.0.0');
        expect(await host.handle({operation: 'resolve', args: ['hasOwnProperty']})).toBe(null);
    });

    it('the Node.js import filter reaches the built-in modules whose names contain digits', () => {
        for (const specifier of ['http2', 'v8', 'node:v8', 'fs/promises', 'worker_threads']) expect(nodeImport.test(specifier)).withContext(specifier).toBeTruthy();
        expect(nodeBuiltin('http2')).toBe('http2');
        expect(nodeBuiltin('v8')).toBe('v8');
    });

    it('the script compiler reads package files on demand, only those it resolves', async () => {
        const lock = lockOf(node('big', '1.0.0', {}, ['package.json', 'index.js', ...Array.from({length: 200}, (_, i) => `lib/${i}.js`)]));
        lock.packages['big@1.0.0'].key = 'big@1.0.0-key';
        const reads = [];
        const readProject = async path => { reads.push(path); return encoder.encode('x'); };
        const {find} = createFinder(null, readProject, new Map(), lock);
        expect(await find('npm', 'big@1.0.0/index')).toEqual({path: 'big@1.0.0/index.js', namespace: 'npm'});
        expect(reads).withContext('only the resolved file is read').toEqual(['.webfront/developer/packages/big@1.0.0-key/index.js']);
    });

    it('context.fetch rejects outside its permissions, accepts a Request and does not follow redirects', async () => {
        const requests = [];
        const realFetch = globalThis.fetch;
        globalThis.fetch = async request => {
            requests.push(request.url);
            return request.url.endsWith('/moved') ? new Response('', {status: 302, headers: {location: 'https://elsewhere.test/'}}) : new Response('ok');
        };
        try {
            const {messages} = await scriptWorker(`export default async context => {
                const outcome = [];
                const refused = context.fetch('https://other.test/');
                outcome.push(refused instanceof Promise);
                await refused.catch(error => outcome.push(error.message));
                outcome.push(await (await context.fetch(new Request('https://api.test/data'))).text());
                await context.fetch('https://api.test/moved').catch(error => outcome.push(error.message));
                context.console.log(JSON.stringify(outcome));
            };`, {fetch: ['https://api.test']});
            const outcome = JSON.parse(messages.find(message => message.type === 'console').text);
            expect(outcome[0]).withContext('a refusal is a rejected promise, not a synchronous exception').toBe(true);
            expect(outcome[1]).toMatch(/Permission denied: fetching https:\/\/other.test/);
            expect(outcome[2]).toBe('ok');
            expect(outcome[3]).toMatch(/does not follow redirects/);
            expect(requests).withContext('the refused origin is never requested').toEqual(['https://api.test/data', 'https://api.test/moved']);
        } finally {
            globalThis.fetch = realFetch;
        }
    });

    it("an error keeps its message when the engine's stack holds only the frames, as in WebKit", async () => {
        const {messages} = await scriptWorker(`export default async () => {
            const error = new Error('Permission denied: writing ui/main.tsx');
            error.stack = 'write@blob:script:1:1';
            throw error;
        };`);
        const failure = messages.find(message => message.type === 'error');
        expect(failure.error).toMatch(/^Error: Permission denied: writing ui\/main.tsx\nwrite@blob:script:1:1$/);
    });
});
