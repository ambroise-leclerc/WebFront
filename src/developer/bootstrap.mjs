import {gunzip, readTar, verifyIntegrity} from '../components/tools/wf.mjs';

export const blockSize = 256 * 1024;
const decoder = new TextDecoder();
export const call = (bridge, name, ...args) => bridge.cppFunction(`webfront.developer.${name}`)(...args);

export async function readProject(bridge, path) {
    const chunks = [];
    let offset = 0;
    for (;;) {
        const chunk = await call(bridge, 'read', path, offset);
        chunks.push(chunk);
        offset += chunk.length;
        if (chunk.length < blockSize) break;
        if (offset > 256 * 1024 * 1024) throw new Error('Project file is too large');
    }
    return new Uint8Array(await new Blob(chunks).arrayBuffer());
}

export async function publish(bridge, destination, files, directory = true, signal, replace = true) {
    const id = await call(bridge, replace ? 'begin' : 'beginNew', destination, directory);
    try {
        for (const [path, bytes] of files) {
            for (let offset = 0; offset < bytes.length || offset === 0; offset += blockSize) {
                signal?.throwIfAborted();
                await call(bridge, 'write', id, path, offset, bytes.subarray(offset, offset + blockSize));
            }
        }
        signal?.throwIfAborted();
        await call(bridge, 'commit', id);
    } catch (error) {
        await call(bridge, 'abort', id).catch(() => {});
        throw error;
    }
}

function safePath(path) {
    return typeof path === 'string' && path.length > 0 && !path.includes('\\') &&
        !path.includes('\0') && !path.split('/').some(part => !part || part === '.' || part === '..');
}

export async function environmentFiles(bytes, integrity) {
    await verifyIntegrity(bytes, integrity);
    const files = new Map();
    for (const entry of readTar(await gunzip(bytes))) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid environment archive root');
        const path = entry.path.slice(8);
        if (!safePath(path) || files.has(path)) throw new Error('Invalid or duplicate environment path');
        files.set(path, entry.bytes);
    }
    if (!files.has('environment.json')) throw new Error('Environment archive has no environment.json');
    const manifest = JSON.parse(decoder.decode(files.get('environment.json')));
    if (manifest.protocol !== 1 || !safePath(manifest.entry) || !files.has(manifest.entry))
        throw new Error('Unsupported WebFront developer environment');
    return {files, manifest};
}


export async function responseBytes(response, limit = 64 * 1024 * 1024) {
    if (!response.ok) throw new Error(`Download: HTTP ${response.status}`);
    if (Number(response.headers.get('content-length')) > limit) throw new Error('Compressed download exceeds its byte limit');
    const reader = response.body.getReader();
    const chunks = [];
    let size = 0;
    try {
        for (;;) {
            const {done, value} = await reader.read();
            if (done) break;
            size += value.length;
            if (size > limit) throw new Error('Compressed download exceeds its byte limit');
            chunks.push(value);
        }
    } catch (error) {
        await reader.cancel().catch(() => {});
        throw error;
    } finally { reader.releaseLock(); }
    return new Uint8Array(await new Blob(chunks).arrayBuffer());
}

export async function bootstrap(bridge, config, fetchImpl = fetch, progress = () => {}) {
    const cached = '.webfront/developer/environment.tgz';
    let bytes;
    try {
        const candidate = await readProject(bridge, cached);
        await environmentFiles(candidate, config.integrity);
        bytes = candidate;
        progress('Using verified cached developer environment');
    } catch {
        progress('Downloading developer environment');
        const url = new URL(config.environment, location.href);
        if (url.protocol !== 'https:' && !(url.origin === location.origin))
            throw new Error('The environment must use HTTPS or the application origin');
        const response = await fetchImpl(url);
        if (!response.ok) throw new Error(`Environment download: HTTP ${response.status}`);
        bytes = await responseBytes(response);
    }
    const {files, manifest} = await environmentFiles(bytes, config.integrity);
    const starters = {};
    for (const [name, starter] of Object.entries(manifest.starters ?? {})) {
        if (!starter || typeof starter.archive !== 'string' || !safePath(starter.archive) ||
            typeof starter.integrity !== 'string' || !starter.integrity.startsWith('sha512-'))
            throw new Error('Invalid pinned starter metadata');
        starters[name] = {url: new URL(starter.archive, new URL(config.environment, location.href)).href, integrity: starter.integrity};
    }
    // All files and the trusted archive are published only after validation succeeds.
    await publish(bridge, '.webfront/developer/environment', files);
    await publish(bridge, cached, new Map([['output', bytes]]), false);
    return {url: `/webfront/developer/environment/${manifest.entry}`, config: {...config, starters}};
}
