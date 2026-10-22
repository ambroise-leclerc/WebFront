// Tests of wf run and wf exec: permissions, command resolution, the page side of the context API and the Worker's
// termination, with a scripted stand-in for the Worker. Apart from node:test and node:assert, they use only browser
// APIs, so that they can move to wf test.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {checkRead, checkWrite, checkFetch, listable, scriptCommand, parseRunOptions, execTarget, lockedPackage, scriptHost,
    runWorker, runResult, runDeadline, secondsLeft, ScriptExit, nodeBuiltin, nodeImport, createFinder, validateManifest}
    from '../environment/packages.bundle.mjs';
import {memoryBridge} from './memory-bridge.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
// The terminal's tokenizer splits on spaces outside quotes; terminal.mjs itself needs xterm.
const tokenize = text => text.match(/"[^"]*"|\S+/g).map(word => word.replace(/^"|"$/g, ''));
const permissions = {files: ['ui', 'data/in.json'], publish: ['generated', '.webfront/x', 'webfront.project.json'], fetch: ['https://api.test']};

test('context operations are checked against the manifest permissions', () => {
    assert.equal(checkRead(permissions, 'ui/main.tsx'), 'ui/main.tsx');
    assert.equal(checkRead(permissions, 'generated/out.txt'), 'generated/out.txt', 'a writable path is readable');
    assert.equal(checkRead(permissions, 'data/in.json'), 'data/in.json');
    for (const path of ['data/other.json', 'uix/a', 'webfront-packages.lock.json'])
        assert.throws(() => checkRead(permissions, path), /Permission denied: reading .* permissions\.files/);
    assert.throws(() => checkRead(permissions, '../outside'), /Not a project-relative path/);
    assert.throws(() => checkRead(permissions, ''), /Not a project-relative path/);
    assert.equal(checkWrite(permissions, 'generated/a/b.txt'), 'generated/a/b.txt');
    assert.throws(() => checkWrite(permissions, 'ui/main.tsx'), /Permission denied: writing ui\/main.tsx .* permissions\.publish/);
    for (const path of ['webfront.project.json', '.webfront/x/y'])
        assert.throws(() => checkWrite(permissions, path), /managed by wf and never written by a script/);
    assert.equal(checkFetch(permissions, 'https://api.test/v1?q=1').href, 'https://api.test/v1?q=1');
    assert.throws(() => checkFetch(permissions, 'https://other.test/'), /Permission denied: fetching https:\/\/other.test/);
    assert.throws(() => checkFetch({}, '/local', 'http://localhost:9002/'), /permissions\.fetch/);
});

test('wf run resolves a script of the manifest to a module or a wf command, without a shell', () => {
    const manifest = validateManifest({format: 1, name: 'app', version: '1.0.0',
        scripts: {gen: 'tools/gen.ts --fast', build: 'wf embed main', loop: 'wf run loop'}});
    assert.deepEqual(scriptCommand(manifest, 'gen', ['a b'], tokenize), {kind: 'module', entry: {namespace: 'project', path: 'tools/gen.ts'}, args: ['--fast', 'a b']});
    assert.deepEqual(scriptCommand(manifest, 'build', ['--x'], tokenize), {kind: 'wf', argv: ['embed', 'main', '--x']});
    assert.throws(() => scriptCommand(manifest, 'missing', [], tokenize), /No script "missing" .* \(scripts: build, gen, loop\)/);
    assert.throws(() => scriptCommand(manifest, 'loop', [], tokenize, 4), /more than 4 levels/);
    assert.throws(() => scriptCommand({...manifest, scripts: {bad: '../x.ts'}}, 'bad', [], tokenize), /Unsafe package path/);
    assert.deepEqual(parseRunOptions(['--timeout', '5', 'gen', '--timeout', '9'], 'u'), {timeout: 5, target: 'gen', args: ['--timeout', '9']});
    assert.equal(parseRunOptions(['gen'], 'u').timeout, 600);
    for (const args of [['--timeout', '0', 'gen'], ['--timeout', '3601', 'gen'], ['--timeout', '1.5', 'gen']])
        assert.throws(() => parseRunOptions(args, 'u'), /--timeout expects whole seconds from 1 to 3600/);
    assert.throws(() => parseRunOptions([], 'wf run <script>'), /Usage: wf run <script>/);
    assert.throws(() => parseRunOptions(['--other', 'gen'], 'wf run <script>'), /Usage/);
});

const node = (name, version, manifest, files = ['package.json', 'bin/tool.mjs']) =>
    ({name, version, manifest: {name, version, ...manifest}, files, dependencies: {}});
const lockOf = (...nodes) => ({format: 1,
    roots: Object.fromEntries(nodes.map(n => [n.name, {requested: n.version, id: `${n.name.replace('/', '+')}@${n.version}`}])),
    packages: Object.fromEntries(nodes.map(n => [`${n.name.replace('/', '+')}@${n.version}`, n]))});

test('wf exec runs only a webfront.bin entry of a locked package', () => {
    const lock = lockOf(
        node('tool', '1.0.0', {webfront: {bin: './bin/tool.mjs'}}),
        node('@scope/multi', '2.0.0', {webfront: {bin: {one: 'bin/tool.mjs', two: 'bin/tool.mjs'}}}),
        node('nodeonly', '1.0.0', {bin: {nodeonly: 'cli.js'}}),
        node('browser', '1.0.0', {main: 'index.js'}),
        node('broken', '1.0.0', {webfront: {bin: 'bin/missing.mjs'}}));
    assert.deepEqual(execTarget(lock, 'tool'), {id: 'tool@1.0.0', name: 'tool', entry: {namespace: 'npm', path: 'tool@1.0.0/bin/tool.mjs'}});
    assert.equal(execTarget(lock, '@scope/multi:two').entry.path, '@scope+multi@2.0.0/bin/tool.mjs');
    assert.throws(() => execTarget(lock, '@scope/multi'), /has the WebFront entries one, two: run wf exec @scope\/multi:<entry>/);
    assert.throws(() => execTarget(lock, 'tool:other'), /has the WebFront entries tool/);
    assert.throws(() => execTarget(lock, 'nodeonly'), /only declares an npm "bin" \(a Node\.js program\)/);
    assert.throws(() => execTarget(lock, 'browser'), /has no WebFront executable entry/);
    assert.throws(() => execTarget(lock, 'broken'), /names a missing file/);
    assert.throws(() => execTarget(lock, 'absent'), /not installed: run wf install absent/);
    assert.throws(() => execTarget(null, 'tool'), /not installed/);
});

test('Node.js built-in modules are recognized, with and without the node: prefix', () => {
    for (const [specifier, name] of [['fs', 'fs'], ['node:fs', 'fs'], ['fs/promises', 'fs'], ['child_process', 'child_process'], ['node:test', 'test']])
        assert.equal(nodeBuiltin(specifier), name);
    for (const specifier of ['react', 'react-dom/client', './fs', 'fsevents']) assert.equal(nodeBuiltin(specifier), null);
});

const project = () => memoryBridge(new Map([
    ['ui/main.tsx', encoder.encode('main')],
    ['ui/parts/a.ts', encoder.encode('a')],
    ['generated/old.txt', encoder.encode('old')],
    ['secret/key', encoder.encode('k')],
]));

test('the page answers context calls within the permissions and publishes each write', async () => {
    const memory = project();
    const host = scriptHost({bridge: memory.bridge, permissions, lock: lockOf(node('tool', '1.0.0', {}))});
    assert.equal(decoder.decode(await host.handle({operation: 'read', args: ['ui/main.tsx']})), 'main');
    assert.deepEqual(await host.handle({operation: 'list', args: ['ui']}), ['main.tsx', 'parts/']);
    await assert.rejects(host.handle({operation: 'read', args: ['secret/key']}), /Permission denied/);
    await assert.rejects(host.handle({operation: 'list', args: ['secret']}), /Permission denied/);
    await host.handle({operation: 'write', args: ['generated/new.txt', encoder.encode('new')]});
    assert.equal(decoder.decode(memory.storage.get('generated/new.txt')), 'new');
    await assert.rejects(host.handle({operation: 'write', args: ['ui/main.tsx', encoder.encode('x')]}), /Permission denied/);
    await assert.rejects(host.handle({operation: 'write', args: ['generated/x', 'text']}), /expects a string or a Uint8Array/);
    assert.deepEqual(await host.handle({operation: 'resolve', args: ['tool']}), {name: 'tool', version: '1.0.0', integrity: undefined});
    assert.equal(await host.handle({operation: 'resolve', args: ['absent']}), null);
    await assert.rejects(host.handle({operation: 'shell', args: []}), /Unknown context operation/);
    assert.deepEqual(host.publications.map(({path, state}) => [path, state]), [['generated/new.txt', 'applied']]);
    await host.stop();
    await assert.rejects(host.handle({operation: 'read', args: ['ui/main.tsx']}), /stopped/);
    assert.equal(memory.pending(), 0);
});

// A bridge that runs `action` once, when the named operation is first called.
function hooked(bridge, operation, action) {
    let fired = false;
    return {cppFunction: name => async (...args) => {
        if (!fired && name.endsWith('.' + operation) && (operation !== 'write' || args[2] > 0)) { fired = true; await action(); }
        return bridge.cppFunction(name)(...args);
    }};
}

test('a stop while a write is staging abandons it: the file is unchanged and no transaction remains', async () => {
    const memory = project();
    let host;
    const bridge = hooked(memory.bridge, 'write', () => { host.stop(); });
    host = scriptHost({bridge, permissions, lock: null});
    const large = new Uint8Array(3 * 256 * 1024).fill(7);
    await assert.rejects(host.handle({operation: 'write', args: ['generated/old.txt', large]}), /stopped/);
    await host.stop();
    assert.equal(decoder.decode(memory.storage.get('generated/old.txt')), 'old');
    assert.equal(memory.pending(), 0, 'the staged transaction is aborted');
    assert.deepEqual(host.publications.map(({state}) => state), ['abandoned']);
});

test('a stop after the commit was sent lets the publication complete with the whole new content', async () => {
    const memory = project();
    let host;
    let stopped;
    const bridge = hooked(memory.bridge, 'commit', () => { stopped = host.stop(); });
    host = scriptHost({bridge, permissions, lock: null});
    const large = new Uint8Array(3 * 256 * 1024).fill(9);
    await host.handle({operation: 'write', args: ['generated/old.txt', large]});
    await stopped;
    assert.deepEqual(memory.storage.get('generated/old.txt'), large);
    assert.deepEqual(host.publications.map(({state}) => state), ['applied']);
    assert.equal(memory.pending(), 0);
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

test('the exit code, console output and context replies of a run', async () => {
    const {worker, output, outcome} = fastRun((message, w) => {
        if (message.type === 'run') {
            w.emit({type: 'console', stream: 'out', text: 'line 1\nline 2'});
            w.emit({type: 'call', id: 1, operation: 'read', args: ['ui/a']});
        }
        if (message.type === 'reply') w.emit({type: 'exit', code: 3});
    });
    assert.deepEqual(await outcome, {reason: 'exit', code: 3});
    assert.deepEqual(output, ['line 1\r\nline 2\r\n']);
    assert.deepEqual(worker.received.find(message => message.type === 'reply'), {type: 'reply', id: 1, value: 'read:ui/a'});
    assert.ok(worker.terminated);
    assert.throws(() => runResult('Script gen', {reason: 'exit', code: 3}, []), error => error instanceof ScriptExit && error.exitCode === 3 && /exited with code 3/.test(error.message));
    assert.equal(runResult('Script gen', {reason: 'exit', code: 0}, []), 'Script gen completed.');
    const failed = await fastRun((message, w) => { if (message.type === 'run') w.emit({type: 'error', error: 'Error: boom'}); }).outcome;
    assert.deepEqual(failed, {reason: 'exit', code: 1, error: 'Error: boom'});
});

test('Ctrl+C aborts the script cooperatively, then terminates one that does not stop', async () => {
    const controller = new AbortController();
    const cooperative = fastRun((message, w) => { if (message.type === 'abort') w.emit({type: 'error', error: 'Error: Interrupted'}); }, {signal: controller.signal});
    controller.abort();
    assert.deepEqual(await cooperative.outcome, {reason: 'interrupted', code: 130, error: 'Error: Interrupted'});
    const busy = new AbortController();
    const looping = fastRun(() => {}, {signal: busy.signal});
    setTimeout(() => busy.abort(), 5);
    const outcome = await looping.outcome;
    assert.deepEqual(outcome, {reason: 'interrupted', code: 130, forced: 20});
    assert.ok(looping.worker.terminated);
    assert.ok(looping.worker.received.some(message => message.type === 'abort'), 'the signal is emitted before the termination');
    const publications = [{path: 'generated/a', state: 'applied'}, {path: 'generated/b', state: 'abandoned'}];
    assert.throws(() => runResult('Script loop', outcome, publications), error => error.exitCode === 130
        && /stopped by Ctrl\+C \(terminated after the 0.02 s grace period\)/.test(error.message)
        && /Applied before the stop: generated\/a/.test(error.message) && /Abandoned, unchanged: generated\/b/.test(error.message));
});

test('the time limit stops a script the same way, without Ctrl+C', async () => {
    const {worker, outcome} = fastRun(() => {}, {timeout: 0.01});
    assert.deepEqual(await outcome, {reason: 'timeout', code: 124, forced: 20});
    assert.ok(worker.terminated && worker.received.some(message => message.type === 'abort'));
    assert.throws(() => runResult('Script loop', {reason: 'timeout', code: 124, forced: 20}, []), /stopped at its time limit/);
});

test('a reply that arrives after the end of the run is not sent to the terminated Worker', async () => {
    let release;
    const host = {handle: () => new Promise(resolve => { release = resolve; })};
    const {worker, outcome} = fastRun((message, w) => {
        if (message.type === 'run') { w.emit({type: 'call', id: 1, operation: 'read', args: []}); w.emit({type: 'exit', code: 0}); }
    }, {host});
    await outcome;
    release('late');
    await new Promise(resolve => setTimeout(resolve, 5));
    assert.equal(worker.received.filter(message => message.type === 'reply').length, 0);
});

test('a refused commit is aborted too, so no transaction or staging is left behind', async () => {
    const memory = project();
    const bridge = {cppFunction: name => name.endsWith('.commit')
        ? async () => { throw new Error('Cannot replace the destination'); }
        : memory.bridge.cppFunction(name)};
    const host = scriptHost({bridge, permissions, lock: null});
    await assert.rejects(host.handle({operation: 'write', args: ['generated/old.txt', encoder.encode('new')]}), /Cannot replace/);
    await host.stop();
    assert.equal(memory.pending(), 0);
    assert.deepEqual(host.publications.map(({state}) => state), ['failed']);
    assert.equal(decoder.decode(memory.storage.get('generated/old.txt')), 'old');
});

// Loads a fresh instance of the real script Worker module, with `self` and module URLs standing in for the browser's.
async function scriptWorker(code, granted = {}) {
    const messages = [];
    const create = URL.createObjectURL;
    globalThis.self = {postMessage: message => messages.push(message)};
    URL.createObjectURL = () => 'data:text/javascript,' + encodeURIComponent(code);
    URL.revokeObjectURL = () => {};
    try {
        await import(`../environment/script-worker.mjs?instance=${Math.random()}`);
        const worker = globalThis.self;
        worker.onmessage({data: {type: 'run', code, args: [], permissions: granted, base: 'http://localhost/'}});
        await new Promise(resolve => setTimeout(resolve, 30));
        return {messages, worker};
    } finally {
        URL.createObjectURL = create;
        delete globalThis.self;
    }
}

test('context.exit stops the script at once, even when it is not awaited', async () => {
    const {messages} = await scriptWorker(`export default async context => {
        context.exit(3);
        context.console.log('ran after exit');
    };`);
    assert.deepEqual(messages, [{type: 'exit', code: 3}]);
    const caught = await scriptWorker(`export default async context => {
        try { context.exit(4); } catch {}
        context.console.log('after a caught exit');
        await context.project.write('generated/x', 'x').catch(error => context.console.log(error.message));
    };`);
    assert.deepEqual(caught.messages, [{type: 'exit', code: 4}], 'nothing reaches the page after the first exit');
});

test('a write the script did not await completes after a normal end, and its failure is reported', async () => {
    const memory = project();
    const host = scriptHost({bridge: memory.bridge, permissions, lock: null});
    const unawaited = host.handle({operation: 'write', args: ['generated/later.txt', encoder.encode('later')]});
    await host.finish();
    await unawaited;
    assert.equal(decoder.decode(memory.storage.get('generated/later.txt')), 'later');
    assert.equal(runResult('Script gen', {reason: 'exit', code: 0}, host.publications), 'Script gen completed.');
    const failing = scriptHost({bridge: {cppFunction: name => name.endsWith('.commit')
        ? async () => { throw new Error('Disk full'); } : memory.bridge.cppFunction(name)}, permissions, lock: null});
    const lost = failing.handle({operation: 'write', args: ['generated/lost.txt', encoder.encode('x')]}).catch(() => {});
    await failing.finish();
    await lost;
    assert.throws(() => runResult('Script gen', {reason: 'exit', code: 0}, failing.publications),
        error => error.exitCode === 1 && /ended without applying all its writes/.test(error.message) && /Failed, unchanged: generated\/lost.txt/.test(error.message));
    // A failure the script awaited is its own to handle: the run still completes.
    const handled = scriptHost({bridge: {cppFunction: name => name.endsWith('.commit')
        ? async () => { throw new Error('Disk full'); } : memory.bridge.cppFunction(name)}, permissions, lock: null});
    await assert.rejects(handled.handle({operation: 'write', args: ['generated/x.txt', encoder.encode('x')]}), /Disk full/);
    await handled.finish();
    assert.equal(runResult('Script gen', {reason: 'exit', code: 0}, handled.publications), 'Script gen completed.');
    assert.equal(memory.pending(), 0);
});

test('the project root and the directories leading to granted paths are listable, filtered', async () => {
    const granted = {files: ['ui/parts'], publish: ['generated']};
    assert.equal(listable(granted, 'ui/parts').keep('anything'), true);
    const root = listable(granted, '');
    assert.deepEqual(['ui/', 'generated/', 'secret/', 'README.md'].filter(root.keep), ['ui/', 'generated/']);
    assert.deepEqual(['parts/', 'main.tsx'].filter(listable(granted, 'ui').keep), ['parts/']);
    assert.equal(listable(granted, '.').path, '');
    assert.throws(() => listable(granted, 'secret'), /Permission denied: listing secret/);
    assert.throws(() => listable({}, ''), /Permission denied: listing the project root/);
    const memory = project();
    const host = scriptHost({bridge: memory.bridge, permissions: granted, lock: null});
    assert.deepEqual(await host.handle({operation: 'list', args: ['']}), ['generated/', 'ui/']);
    assert.deepEqual(await host.handle({operation: 'list', args: []}), ['generated/', 'ui/'], 'list() lists the root');
    assert.deepEqual(await host.handle({operation: 'list', args: ['ui']}), ['parts/']);
});

test('a time limit also bounds the scripts run through wf commands', () => {
    const outer = runDeadline(5, undefined, 1000);
    assert.equal(outer, 6000);
    assert.equal(runDeadline(600, outer, 2000), 6000, 'a nested run keeps the earlier deadline');
    assert.equal(runDeadline(1, outer, 2000), 3000, 'and may set an earlier one');
    assert.equal(secondsLeft(6000, 2000), 4);
    assert.equal(secondsLeft(6000, 7000), 0);
});

test('a locked name is looked up as an own key, and resolve() designates the package wf exec uses', async () => {
    const lock = lockOf(node('tool', '2.0.0', {webfront: {bin: 'bin/tool.mjs'}}));
    lock.packages['tool@1.0.0'] = node('tool', '1.0.0', {});
    assert.throws(() => execTarget(lock, 'constructor'), /constructor is not installed/);
    assert.equal(lockedPackage(lock, 'toString'), null);
    assert.equal(lockedPackage(lock, 'tool')[0], 'tool@2.0.0');
    const host = scriptHost({bridge: project().bridge, permissions, lock});
    assert.equal((await host.handle({operation: 'resolve', args: ['tool']})).version, '2.0.0', 'the root, not a transitive version');
    assert.equal(await host.handle({operation: 'resolve', args: ['hasOwnProperty']}), null);
});

test('the Node.js import filter reaches the built-in modules whose names contain digits', () => {
    for (const specifier of ['http2', 'v8', 'node:v8', 'fs/promises', 'worker_threads']) assert.ok(nodeImport.test(specifier), specifier);
    assert.equal(nodeBuiltin('http2'), 'http2');
    assert.equal(nodeBuiltin('v8'), 'v8');
});

test('the script compiler reads package files on demand, only those it resolves', async () => {
    const lock = lockOf(node('big', '1.0.0', {}, ['package.json', 'index.js', ...Array.from({length: 200}, (_, i) => `lib/${i}.js`)]));
    lock.packages['big@1.0.0'].key = 'big@1.0.0-key';
    const reads = [];
    const readProject = async path => { reads.push(path); return encoder.encode('x'); };
    const {find} = createFinder(null, readProject, new Map(), lock);
    assert.deepEqual(await find('npm', 'big@1.0.0/index'), {path: 'big@1.0.0/index.js', namespace: 'npm'});
    assert.deepEqual(reads, ['.webfront/developer/packages/big@1.0.0-key/index.js'], 'only the resolved file is read');
});

test('context.fetch rejects outside its permissions, accepts a Request and does not follow redirects', async () => {
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
        assert.equal(outcome[0], true, 'a refusal is a rejected promise, not a synchronous exception');
        assert.match(outcome[1], /Permission denied: fetching https:\/\/other.test/);
        assert.equal(outcome[2], 'ok');
        assert.match(outcome[3], /does not follow redirects/);
        assert.deepEqual(requests, ['https://api.test/data', 'https://api.test/moved'], 'the refused origin is never requested');
    } finally {
        globalThis.fetch = realFetch;
    }
});

test("an error keeps its message when the engine's stack holds only the frames, as in WebKit", async () => {
    const {messages} = await scriptWorker(`export default async () => {
        const error = new Error('Permission denied: writing ui/main.tsx');
        error.stack = 'write@blob:script:1:1';
        throw error;
    };`);
    const failure = messages.find(message => message.type === 'error');
    assert.match(failure.error, /^Error: Permission denied: writing ui\/main.tsx\nwrite@blob:script:1:1$/);
});
