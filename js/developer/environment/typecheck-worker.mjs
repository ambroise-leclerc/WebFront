// The Worker of wf check: loads the locked TypeScript, then the snapshot through the page (phase 1, asynchronous),
// then compiles over it (phase 2, synchronous). The page answers only reads and listings: nothing is written.
import {packageRoot} from './paths.mjs';
import {typescriptModule, loadSnapshot, checkProgram, checkReport, snapshotBounds} from './typecheck-snapshot.mjs';

const pending = new Map();
let nextId = 0;
const controller = new AbortController();
const decoder = new TextDecoder();

function call(operation, ...args) {
    return new Promise((resolve, reject) => {
        const id = ++nextId;
        pending.set(id, {resolve, reject});
        self.postMessage({type: 'call', id, operation, args});
    });
}

const print = (text, stream = 'out') => self.postMessage({type: 'console', stream, text});

async function loadTypeScript(node) {
    const source = decoder.decode(await call('read', `${packageRoot}/${node.key}/lib/typescript.js`));
    const url = URL.createObjectURL(new Blob([typescriptModule(source)], {type: 'text/javascript'}));
    let ts;
    try { ts = (await import(url)).default; }
    finally { URL.revokeObjectURL(url); }
    if (ts?.version !== node.version || typeof ts.createProgram !== 'function' || typeof ts.matchFiles !== 'function')
        throw new Error(`typescript@${node.version}: lib/typescript.js does not provide the compiler API wf check uses`);
    return ts;
}

async function run({lock, typescript, entries, bounds: given}) {
    const bounds = given ?? snapshotBounds;
    const ts = await loadTypeScript(lock.packages[typescript]);
    const loaded = await loadSnapshot(ts, {read: path => call('read', path), list: directory => call('list', directory),
        lock, typescript, entries, bounds, signal: controller.signal});
    controller.signal.throwIfAborted();
    const {snapshot, rootNames} = loaded;
    print(`Checking ${rootNames.length} root file${rootNames.length === 1 ? '' : 's'} with TypeScript ${ts.version} ` +
        `(snapshot: ${snapshot.files.size} files, ${(snapshot.bytes / 1048576).toFixed(1)} MiB)`);
    // Synchronous from here: Ctrl+C can only terminate the Worker.
    const {lines, code} = checkReport(ts, snapshot, checkProgram(ts, loaded, typescript), bounds);
    for (const line of lines) print(line, code ? 'err' : 'out');
    self.postMessage({type: 'exit', code});
}

function answer({id, error, value}) {
    const request = pending.get(id);
    if (!request) return;
    pending.delete(id);
    if (error === undefined) request.resolve(value);
    else request.reject(new Error(error));
}

self.onmessage = ({data}) => {
    if (data.type === 'reply') return answer(data);
    if (data.type === 'abort') return controller.abort(new Error('Interrupted'));
    if (data.type !== 'run') return;
    run(data).catch(error => self.postMessage({type: 'error', error: error.message}));
};
