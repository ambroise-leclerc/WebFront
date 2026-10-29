// Browser builds and development loop. Only project-relative bridge reads; no platform watcher or shell.
import {call, publish} from './io.mjs';
import {loadProject, resolveEntry, syncNotice, noManifest} from './project.mjs';
import {manifestPath, lockPath} from './manifest.mjs';
import {applicationPage} from './embed.mjs';

export const watchBounds = {paths: 1024, bytes: 32 * 1024 * 1024, interval: 750};
const stopped = () => Object.assign(new Error('Interrupted'), {exitCode: 130});
const digest = async bytes => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), b => b.toString(16).padStart(2, '0')).join('');

// Bound both each read and the whole pass, including resolution probes for files that do not exist yet.
export async function observedRead(bridge, path, observed, budget, signal) {
    signal?.throwIfAborted();
    if (!observed.has(path) && observed.size >= watchBounds.paths) throw new Error('Build/watch exceeds 1024 input paths');
    observed.set(path, null);
    const chunks = [];
    let offset = 0;
    try {
        for (;;) {
            signal?.throwIfAborted();
            const chunk = await call(bridge, 'read', path, offset);
            budget.bytes += chunk.length;
            if (budget.bytes > watchBounds.bytes) throw new Error('Build/watch exceeds 32 MiB of input per pass');
            chunks.push(chunk);
            offset += chunk.length;
            if (chunk.length < 256 * 1024) break;
        }
    } catch (error) {
        // Other read failures must be retried/reported, rather than mistaken for a missing import.
        observed.set(path, error.message.includes('Cannot read project file:') ? 'missing' : `error:${error.message}`);
        throw error;
    }
    const bytes = new Uint8Array(await new Blob(chunks).arrayBuffer());
    observed.set(path, await digest(bytes));
    return bytes;
}

/** A persistent compiler Worker. At most one rebuild at a time; stop/timeout terminate it and reject pending work. */
export function compilerSession(bridge, signal, createWorker = () => new Worker(new URL('./compiler.mjs', import.meta.url), {type: 'module'})) {
    const worker = createWorker();
    let pending, closed = false, observed = new Map(), budget;
    const finish = (error, files) => {
        if (!pending) return;
        const {resolve, reject, timer} = pending;
        pending = null;
        clearTimeout(timer);
        error ? reject(error) : resolve(files);
    };
    const close = (error = stopped()) => {
        if (closed) return;
        closed = true;
        worker.terminate();
        signal?.removeEventListener('abort', abort);
        finish(error);
    };
    const abort = () => close(stopped());
    signal?.addEventListener('abort', abort, {once: true});
    worker.onerror = event => close(new Error(event.message || 'Compiler worker failed'));
    worker.onmessage = async ({data}) => {
        if (closed || !pending) return;
        if (data.type === 'read') {
            let response;
            try { response = {bytes: await observedRead(bridge, data.path, observed, budget, signal)}; }
            catch (error) {
                if (/Build\/watch exceeds/.test(error.message)) return close(error);
                response = {error: error.message};
            }
            if (!closed && pending) worker.postMessage({type: 'read-result', id: data.id, ...response});
        } else if (data.type === 'result') finish(null, data.files);
        else if (data.type === 'error') finish(Object.assign(new Error(data.error), {diagnostics: data.diagnostics}));
    };
    if (signal?.aborted) abort();
    return {
        get closed() { return closed; },
        get observed() { return observed; },
        rebuild(message) {
            if (closed) return Promise.reject(stopped());
            if (pending) return Promise.reject(new Error('A browser build is already running'));
            observed = new Map();
            budget = {bytes: 0};
            return new Promise((resolve, reject) => {
                pending = {resolve, reject, timer: setTimeout(() => close(new Error('Browser build timed out')), 120000)};
                worker.postMessage({type: 'build', incremental: true, ...message});
            });
        },
        close,
    };
}

export function diagnosticsText(error) {
    if (!error.diagnostics?.length) return error.message;
    return error.diagnostics.map(({text, location}) => location ?
        `${location.file.replace(/^project:/, '')}:${location.line}:${location.column + 1}: ${text}\n${location.lineText ?? ''}` : text).join('\n');
}

// textContent keeps a diagnostic containing source text inert in the application's document.
export function previewDiagnostics(frame, text) {
    if (text && frame) frame.hidden = false;
    const document = frame?.contentDocument;
    if (!document?.body) return;
    document.getElementById('webfront-build-diagnostics')?.remove();
    if (!text) return;
    const overlay = document.createElement('pre');
    overlay.id = 'webfront-build-diagnostics';
    overlay.setAttribute('role', 'alert');
    overlay.style.cssText = 'position:fixed;inset:0;z-index:2147483647;margin:0;padding:24px;overflow:auto;white-space:pre-wrap;background:#211821ed;color:#ffd8d8;font:14px/1.5 monospace';
    overlay.textContent = text;
    document.body.append(overlay);
}

let revision = 0;
export function showPreview(frame, directory = 'preview') {
    if (!frame) throw new Error('No preview frame is available');
    frame.hidden = false;
    const id=++revision;
    frame.dispatchEvent?.(new CustomEvent('webfront-preview-navigating',{detail:{id}}));
    try {
        frame.contentWindow.location.replace(`/webfront/developer/${directory}/index.html?build=${Date.now()}-${id}`);
    } catch (error) {
        frame.dispatchEvent?.(new CustomEvent('webfront-preview-failed',{detail:{id}}));
        throw error;
    }
}

// Manifest labels allow punctuation that project paths forbid. Escape it without collisions; also avoid
// Windows device directory names, so the same manifest builds on every supported platform.
function entryDirectory(name) {
    const escaped = encodeURIComponent(name).replaceAll('.', '%2E');
    return /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i.test(escaped) ?
        `%${escaped.charCodeAt(0).toString(16).toUpperCase()}${escaped.slice(1)}` : escaped;
}

export function buildEntries(manifest, argument) {
    if (argument !== undefined) return [['', resolveEntry(manifest, argument)]];
    const entries = Object.entries(manifest?.entries ?? {});
    if (!entries.length) throw new Error('Usage: wf build <entry> (or declare entries in webfront.project.json)');
    return entries.map(([name, entry]) => [entries.length === 1 ? '' : entryDirectory(name), entry]);
}

export async function build(args, {bridge, signal}) {
    if (args.length > 1) throw new Error('Usage: wf build [entry.tsx|name]');
    signal?.throwIfAborted();
    const project = await loadProject(bridge);
    const session = compilerSession(bridge, signal);
    try {
        const files = new Map();
        for (const [name, entry] of buildEntries(project.manifest, args[0])) {
            const output = await session.rebuild({entry, lock: project.lock ?? {format: 1, roots: {}, packages: {}}});
            output.set('index.html', applicationPage(output));
            for (const [path, bytes] of output) files.set(name ? `${name}/${path}` : path, bytes);
        }
        await publish(bridge, '.webfront/developer/build', files, {signal});
        return syncNotice(project) + 'Build ready: .webfront/developer/build/';
    } catch (error) {
        if (signal?.aborted) throw stopped();
        throw Object.assign(error, {message: diagnosticsText(error)});
    } finally { session.close(); }
}

export function waitForPoll(signal, milliseconds = watchBounds.interval) {
    return new Promise((resolve, reject) => {
        const abort = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); reject(stopped()); };
        const timer = setTimeout(() => { signal?.removeEventListener('abort', abort); resolve(); }, milliseconds);
        signal?.addEventListener('abort', abort, {once: true});
        if (signal?.aborted) abort();
    });
}

// Compare to the bytes actually read by the build, not a snapshot taken after it: edits made during compilation
// are still noticed. Missing candidates are watched too, so creating an unresolved import recovers a failed build.
export async function inputsChanged(bridge, observed, signal) {
    const current = new Map(), budget = {bytes: 0};
    for (const [path, before] of observed) {
        try { await observedRead(bridge, path, current, budget, signal); }
        catch (error) {
            if (signal?.aborted) throw stopped();
            if (current.get(path) !== 'missing') throw error;
        }
        if (current.get(path) !== before) return true;
    }
    return false;
}

export async function dev(args, {bridge, signal, preview: frame, write = () => {}, withBuild = action => action()}) {
    if (args.length > 1) throw new Error('Usage: wf dev [entry.tsx|name]');
    const controller = new AbortController();
    const abort = () => controller.abort();
    let connectionClosed = false;
    const disconnected = () => { connectionClosed = true; controller.abort(); };
    signal?.addEventListener('abort', abort, {once: true});
    bridge.socket?.addEventListener('close', disconnected, {once: true});
    if (signal?.aborted) abort();
    const active = controller.signal;
    let session, observed = new Map();
    let diagnostic = '';
    const loaded = () => previewDiagnostics(frame, diagnostic);
    frame?.addEventListener('load', loaded);
    try {
        for (;;) {
            active.throwIfAborted();
            const configReads = new Map(), budget = {bytes: 0};
            try {
                await withBuild(async () => {
                    // Record the configuration before loading it. A concurrent edit triggers another iteration.
                    for (const path of [manifestPath, lockPath]) {
                        try { await observedRead(bridge, path, configReads, budget, active); }
                        catch (error) { if (configReads.get(path) !== 'missing') throw error; }
                    }
                    const project = await loadProject(bridge);
                    if (!project.manifest && args[0] === undefined) throw new Error(noManifest);
                    const entry = resolveEntry(project.manifest, args[0], 'wf dev <entry.tsx>');
                    if (!session || session.closed) session = compilerSession(bridge, active);
                    const files = await session.rebuild({entry, lock: project.lock ?? {format: 1, roots: {}, packages: {}}});
                    files.set('index.html', applicationPage(files));
                    await publish(bridge, '.webfront/developer/preview', files, {signal: active});
                    // Once commit succeeds, display its publication even if Ctrl+C arrived while awaiting it.
                    diagnostic = '';
                    previewDiagnostics(frame, '');
                    showPreview(frame);
                    // A successful build can drop dependencies that are no longer imported.
                    observed = new Map(session.observed);
                    write(syncNotice(project) + 'Preview ready. Watching inputs (Ctrl+C to stop).\n');
                });
            } catch (error) {
                if (active.aborted) throw stopped();
                if (error?.code === 'busy') { await waitForPoll(active); continue; }
                diagnostic = diagnosticsText(error);
                write(diagnostic + '\n');
                previewDiagnostics(frame, diagnostic);
                if (session?.observed.size) observed = new Map(session.observed);
            }
            observed = new Map([...configReads, ...[...observed].filter(([path]) => !configReads.has(path))]);
            if (observed.size > watchBounds.paths) throw new Error('Build/watch exceeds 1024 input paths');
            for (;;) {
                await waitForPoll(active);
                try { if (await inputsChanged(bridge, observed, active)) break; }
                catch (error) {
                    if (active.aborted) throw stopped();
                    // A temporarily unreadable file or a bound violation cannot publish or reload anything.
                    const text = diagnosticsText(error);
                    if (diagnostic !== text) { diagnostic = text; write(text + '\n'); previewDiagnostics(frame, text); }
                }
            }
        }
    } catch (error) {
        if (controller.signal.aborted) {
            if (connectionClosed) throw new Error('The connection to the application closed');
            throw stopped();
        }
        throw error;
    } finally {
        session?.close();
        signal?.removeEventListener('abort', abort);
        bridge.socket?.removeEventListener('close', disconnected);
        frame?.removeEventListener('load', loaded);
        previewDiagnostics(frame, '');
    }
}
