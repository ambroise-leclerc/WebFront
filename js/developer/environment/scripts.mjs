// wf run and wf exec: WebFront scripts run in a dedicated Worker; the page answers their context calls under the
// manifest permissions, publishes with developer::Store and stops the Worker on Ctrl+C or at the time limit.
import {call, readProject, writeChunks} from './io.mjs';
import {checkRead, checkWrite, listable} from './permissions.mjs';
import {manifestPath} from './manifest.mjs';
import {safePackagePath} from './paths.mjs';

export const gracePeriod = 2000;
export const defaultTimeout = 600;
export const timeoutBounds = [1, 3600];
const maxWfDepth = 4;

/** An error whose exitCode is the script's exit code, for the terminal and non-interactive callers. */
export class ScriptExit extends Error {
    constructor(message, exitCode) {
        super(message);
        this.exitCode = exitCode;
    }
}

/** The deadline of a run: its own limit, never later than the deadline of the wf script that started it. */
export const runDeadline = (timeout, inherited = Infinity, now = Date.now()) => Math.min(now + timeout * 1000, inherited);
export const secondsLeft = (deadline, now = Date.now()) => Math.max(0, (deadline - now) / 1000);

// [--timeout <seconds>] before the script or package; everything after it belongs to the script.
export function parseRunOptions(args, usage) {
    let timeout = defaultTimeout;
    let rest = args;
    if (rest[0] === '--timeout') {
        timeout = Number(rest[1]);
        if (!Number.isInteger(timeout) || timeout < timeoutBounds[0] || timeout > timeoutBounds[1])
            throw new Error(`--timeout expects whole seconds from ${timeoutBounds[0]} to ${timeoutBounds[1]}`);
        rest = rest.slice(2);
    }
    if (!rest.length || rest[0].startsWith('-')) throw new Error(`Usage: ${usage}`);
    return {timeout, target: rest[0], args: rest.slice(1)};
}

/**
 * A script of the manifest: "wf <command> ..." runs a wf command, otherwise the first word is the project path of a
 * JS/TS module and the other words are its first arguments. No shell is involved.
 */
export function scriptCommand(manifest, name, args, tokenize, depth = 0) {
    const scripts = manifest?.scripts ?? {};
    if (!Object.hasOwn(scripts, name)) {
        const known = Object.keys(scripts).sort().join(', ') || 'none';
        throw new Error(`No script "${name}" in ${manifestPath} (scripts: ${known})`);
    }
    const [first, ...words] = tokenize(scripts[name]);
    if (first === 'wf') {
        if (depth >= maxWfDepth) throw new Error(`Scripts call wf more than ${maxWfDepth} levels deep`);
        return {kind: 'wf', argv: [...words, ...args]};
    }
    return {kind: 'module', entry: {namespace: 'project', path: safePackagePath(first)}, args: [...words, ...args]};
}

/** The locked package a name designates, as [id, node]: the root of that name, else its only version; null if absent. */
export function lockedPackage(lock, name) {
    if (lock?.roots && Object.hasOwn(lock.roots, name)) {
        const {id} = lock.roots[name];
        return [id, lock.packages[id]];
    }
    const found = Object.entries(lock?.packages ?? {}).filter(([, node]) => node.name === name);
    if (found.length > 1) throw new Error(`${name} is locked at several versions; install it as a root to choose one`);
    return found[0] ?? null;
}

function binEntries(id, manifest) {
    const bin = manifest.webfront?.bin;
    if (typeof bin === 'string') return {[manifest.name.split('/').pop()]: bin};
    if (bin && typeof bin === 'object' && !Array.isArray(bin)) return bin;
    if (manifest.bin) throw new Error(`${id} only declares an npm "bin" (a Node.js program); wf exec runs only "webfront.bin" entries`);
    throw new Error(`${id} has no WebFront executable entry ("webfront.bin" in its package.json)`);
}

/** wf exec <pkg>[:<name>]: the "webfront.bin" entry of a locked package; npm's "bin" is never run. */
export function execTarget(lock, spec) {
    const colon = spec.indexOf(':', spec.startsWith('@') ? spec.indexOf('/') : 0);
    const [name, entryName] = colon < 0 ? [spec, undefined] : [spec.slice(0, colon), spec.slice(colon + 1)];
    const locked = lockedPackage(lock, name);
    if (!locked) throw new Error(`${name} is not installed: run wf install ${name}`);
    const [id, node] = locked;
    const entries = binEntries(id, node.manifest);
    const names = Object.keys(entries);
    const chosen = entryName ?? (names.length === 1 ? names[0] : undefined);
    if (!chosen || !Object.hasOwn(entries, chosen))
        throw new Error(`${id} has the WebFront entries ${names.join(', ') || 'none'}: run wf exec ${name}:<entry>`);
    const path = String(entries[chosen]).replace(/^\.\//, '');
    if (!node.files.includes(safePackagePath(path))) throw new Error(`${id}: webfront.bin entry ${chosen} names a missing file ${path}`);
    return {id, name: chosen, entry: {namespace: 'npm', path: `${id}/${path}`}};
}

const decoder = new TextDecoder();

/**
 * The page side of a run: answers the Worker's context calls. Each write is a transaction of its own. When the
 * script ends by itself, finish() lets the writes it did not await complete; after stop() (Ctrl+C, time limit), a
 * transaction still staging is aborted, and one whose commit was sent completes or fails atomically in C++.
 */
export function scriptHost({bridge, permissions, lock}) {
    const controller = new AbortController();
    const publications = [];
    const inflight = new Set();

    async function write(path, bytes) {
        const target = checkWrite(permissions, path);
        if (!(bytes instanceof Uint8Array)) throw new TypeError('context.project.write expects a string or a Uint8Array');
        const publication = {path: target, state: 'staging'};
        publications.push(publication);
        let id = null;
        try {
            id = await call(bridge, 'begin', target, false);
            await writeChunks(bridge, {id, path: 'output', bytes}, controller.signal);
            controller.signal.throwIfAborted();
            publication.state = 'committing';
            await call(bridge, 'commit', id);
            publication.state = 'applied';
        } catch (error) {
            // A refused commit keeps its transaction in the store: it is aborted too, so its staging is removed.
            // Abandoned: staged when the run was stopped; failed: refused by the store, or never staged.
            const stopped = controller.signal.aborted && publication.state !== 'committing';
            publication.state = stopped ? 'abandoned' : 'failed';
            if (id !== null) await call(bridge, 'abort', id).catch(() => {});
            throw error;
        }
    }

    const operations = {
        read: path => readProject(bridge, checkRead(permissions, path)),
        list: async (directory = '') => {
            const {path, keep} = listable(permissions, directory);
            return JSON.parse(decoder.decode(await call(bridge, 'list', path))).filter(keep);
        },
        write,
        // The package wf exec and the bundle would use: the root of that name, else its only locked version.
        resolve: name => {
            const node = lockedPackage(lock, name)?.[1];
            return node ? {name: node.name, version: node.version, integrity: node.integrity} : null;
        },
    };

    return {
        publications,
        async handle({operation, args}) {
            if (controller.signal.aborted) throw new Error('The script was stopped');
            if (!Object.hasOwn(operations, operation)) throw new Error(`Unknown context operation: ${operation}`);
            const promise = operations[operation](...args);
            inflight.add(promise);
            try { return await promise; } finally { inflight.delete(promise); }
        },
        // After a normal end: the writes the script did not await complete, then the host stops answering. Those
        // writes are marked, since the script never learnt whether they failed.
        async finish() {
            for (const publication of publications)
                if (publication.state === 'staging' || publication.state === 'committing') publication.unawaited = true;
            while (inflight.size) await Promise.allSettled([...inflight]);
            await this.stop();
        },
        // Stops answering, aborts the transactions still staging and waits for those already committing.
        async stop() {
            controller.abort(new Error('The script was stopped'));
            await Promise.allSettled([...inflight]);
        },
    };
}

/**
 * Runs compiled script code in a Worker. Ctrl+C (signal) or the time limit first aborts the script's context.signal;
 * a script still running after the grace period is terminated with worker.terminate(), which also stops one that
 * never returns to its event loop. Resolves with {code, reason, error}; reason is 'exit', 'interrupted' or 'timeout'.
 */
export function runWorker({createWorker, host, message, signal, timeout = defaultTimeout, grace = gracePeriod, write}) {
    return new Promise(resolve => {
        const worker = createWorker();
        const timers = [];
        let stopping = null;
        let done = false;
        const stoppedCode = () => stopping === 'timeout' ? 124 : 130;
        const finish = outcome => {
            if (done) return;
            done = true;
            worker.terminate();
            timers.forEach(clearTimeout);
            signal?.removeEventListener('abort', interrupt);
            resolve({reason: stopping ?? 'exit', ...outcome});
        };
        const stop = reason => {
            if (stopping || done) return;
            stopping = reason;
            worker.postMessage({type: 'abort'});
            timers.push(setTimeout(() => finish({code: stoppedCode(), forced: grace}), grace));
        };
        const interrupt = () => stop('interrupted');
        const reply = async data => {
            let answer;
            try { answer = {value: await host.handle(data)}; }
            catch (error) { answer = {error: error.message}; }
            if (!done) worker.postMessage({type: 'reply', id: data.id, ...answer});
        };
        // A script that ends by itself after a stop request keeps the stop's code unless it chose another one.
        const handlers = {
            console: data => write(String(data.text).replace(/\r?\n/g, '\r\n') + '\r\n', data.stream),
            exit: data => finish({code: stopping && data.code === 0 ? stoppedCode() : data.code}),
            error: data => finish({code: stopping ? stoppedCode() : 1, error: data.error}),
            call: reply,
        };
        worker.onmessage = ({data}) => handlers[data?.type]?.(data);
        worker.onerror = event => {
            event.preventDefault?.();
            finish({code: 1, error: event.message || 'The script Worker failed'});
        };
        signal?.addEventListener('abort', interrupt, {once: true});
        timers.push(setTimeout(() => stop('timeout'), timeout * 1000));
        worker.postMessage({type: 'run', ...message});
        if (signal?.aborted) interrupt();
    });
}

// After a stop, every write is reported; after a normal end, only the failures of the writes the script did not await.
function publicationReport(all, stopped) {
    const publications = stopped ? all : all.filter(publication => publication.unawaited);
    const by = state => publications.filter(publication => publication.state === state).map(({path}) => path);
    const lines = [];
    if (stopped && by('applied').length) lines.push(`Applied before the stop: ${by('applied').join(', ')}`);
    if (by('abandoned').length) lines.push(`Abandoned, unchanged: ${by('abandoned').join(', ')}`);
    if (by('failed').length) lines.push(`Failed, unchanged: ${by('failed').join(', ')}`);
    return lines;
}

/**
 * The terminal's result of a run: a summary when the script ended with code 0 and every write was applied, a
 * ScriptExit otherwise. A write that was not applied, even one the script did not await, is never silent.
 */
export function runResult(label, outcome, publications) {
    const stopped = {interrupted: 'stopped by Ctrl+C', timeout: 'stopped at its time limit'}[outcome.reason];
    const report = publicationReport(publications, stopped !== undefined);
    if (outcome.code === 0 && !stopped && !report.length) return `${label} completed.`;
    const forced = outcome.forced ? ` (terminated after the ${outcome.forced / 1000} s grace period)` : '';
    const code = outcome.code === 0 && !stopped ? 1 : outcome.code;
    const how = stopped ? `${stopped}${forced}` : outcome.code === 0 ? 'ended without applying all its writes' : `exited with code ${outcome.code}`;
    throw new ScriptExit([outcome.error, `${label} ${how}`, ...report].filter(Boolean).join('\r\n'), code);
}
