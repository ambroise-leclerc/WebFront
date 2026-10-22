// The Worker of a WebFront script: loads the compiled module and calls its default export with the context API.
// Project files go through the page, which applies the manifest permissions and publishes with developer::Store.
import {checkFetch} from './permissions.mjs';

const pending = new Map();
let nextId = 0;
const controller = new AbortController();
// Set by the first exit: nothing the script does afterwards reaches the page.
let exited = false;

function post(message) {
    if (!exited) self.postMessage(message);
}

function call(operation, ...args) {
    if (exited) return Promise.reject(new Error('The script has exited'));
    return new Promise((resolve, reject) => {
        const id = ++nextId;
        pending.set(id, {resolve, reject});
        self.postMessage({type: 'call', id, operation, args});
    });
}

function format(value) {
    if (typeof value === 'string') return value;
    if (value instanceof Error) return value.stack ?? String(value);
    try { return JSON.stringify(value) ?? String(value); } catch { return String(value); }
}

const print = stream => (...values) => post({type: 'console', stream, text: values.map(format).join(' ')});
const scriptConsole = {log: print('out'), info: print('out'), debug: print('out'), warn: print('err'), error: print('err')};

// Thrown by exit() to unwind the script at once; the page also terminates the Worker on the exit message.
class Exit {
    constructor(code) { this.code = code; }
}

// Ends the run with a code. The statements after exit() do not run, unless the script catches everything it throws;
// even then, nothing more reaches the page: no output, no context operation.
function exit(code = 0) {
    if (!Number.isInteger(code) || code < 0 || code > 255) throw new RangeError('context.exit expects an integer from 0 to 255');
    post({type: 'exit', code});
    exited = true;
    throw new Exit(code);
}

const decoder = new TextDecoder();
const redirected = response => response.type === 'opaqueredirect' || (response.status >= 300 && response.status < 400 && response.headers.has('location'));

// fetch to an origin of permissions.fetch. A URL or a Request is accepted; a refusal rejects like the other context
// operations. Redirects are not followed, since their target could be any origin: the caller requests the final URL.
async function contextFetch(permissions, base, input, init = {}) {
    if (exited) throw new Error('The script has exited');
    const target = checkFetch(permissions, input instanceof Request ? input.url : input, base);
    const request = new Request(input instanceof Request ? input : target, {...init, redirect: 'manual', signal: init.signal ?? controller.signal});
    const response = await fetch(request);
    if (redirected(response))
        throw new Error(`context.fetch does not follow redirects: ${target.href} redirects; request the final URL, whose origin permissions.fetch must list`);
    return response;
}

function createContext({args, permissions, base}) {
    return Object.freeze({
        args: Object.freeze([...args]),
        signal: controller.signal,
        console: scriptConsole,
        project: Object.freeze({
            read: path => call('read', path),
            readText: async path => decoder.decode(await call('read', path)),
            write: (path, data) => call('write', path, typeof data === 'string' ? new TextEncoder().encode(data) : data),
            list: (path = '') => call('list', path),
        }),
        packages: Object.freeze({resolve: name => call('resolve', name)}),
        fetch: (input, init) => contextFetch(permissions, base, input, init),
        exit,
    });
}

async function run(message) {
    const url = URL.createObjectURL(new Blob([message.code], {type: 'text/javascript'}));
    let module;
    try { module = await import(url); }
    finally { URL.revokeObjectURL(url); }
    if (typeof module.default !== 'function') throw new Error('A WebFront script must export default async function (context)');
    await module.default(createContext(message));
    post({type: 'exit', code: 0});
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
    // Libraries that log through the global console reach the terminal too.
    Object.assign(globalThis.console, scriptConsole);
    run(data).catch(error => {
        if (!(error instanceof Exit)) post({type: 'error', error: format(error), interrupted: controller.signal.aborted});
    });
};
