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

async function writeChunks(bridge, id, path, bytes, signal) {
    for (let offset = 0; offset < bytes.length || offset === 0; offset += blockSize) {
        signal?.throwIfAborted();
        await call(bridge, 'write', id, path, offset, bytes.subarray(offset, offset + blockSize));
    }
}

export async function publish(bridge, destination, files, {directory = true, signal, replace = true} = {}) {
    const id = await call(bridge, replace ? 'begin' : 'beginNew', destination, directory);
    try {
        for (const [path, bytes] of files) await writeChunks(bridge, id, path, bytes, signal);
        signal?.throwIfAborted();
        await call(bridge, 'commit', id);
    } catch (error) {
        await call(bridge, 'abort', id).catch(() => {});
        throw error;
    }
}

function safePath(path) {
    if (typeof path !== 'string' || !path) return false;
    if (path.includes('\\') || path.includes('\0')) return false;
    return !path.split('/').some(part => !part || part === '.' || part === '..');
}

function archiveFiles(tarball) {
    const files = new Map();
    for (const entry of readTar(tarball)) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid environment archive root');
        const path = entry.path.slice(8);
        if (!safePath(path) || files.has(path)) throw new Error('Invalid or duplicate environment path');
        files.set(path, entry.bytes);
    }
    return files;
}

export async function environmentFiles(bytes, integrity) {
    await verifyIntegrity(bytes, integrity);
    const files = archiveFiles(await gunzip(bytes));
    if (!files.has('environment.json')) throw new Error('Environment archive has no environment.json');
    const manifest = JSON.parse(decoder.decode(files.get('environment.json')));
    if (manifest.protocol !== 1 || !safePath(manifest.entry) || !files.has(manifest.entry))
        throw new Error('Unsupported WebFront developer environment');
    return {files, manifest};
}

async function readLimited(reader, limit) {
    const chunks = [];
    let size = 0;
    try {
        for (;;) {
            const {done, value} = await reader.read();
            if (done) return chunks;
            size += value.length;
            if (size > limit) throw new Error('Compressed download exceeds its byte limit');
            chunks.push(value);
        }
    } catch (error) {
        await reader.cancel().catch(() => {});
        throw error;
    } finally { reader.releaseLock(); }
}

export async function responseBytes(response, limit = 64 * 1024 * 1024) {
    if (!response.ok) throw new Error(`Download: HTTP ${response.status}`);
    if (Number(response.headers.get('content-length')) > limit) throw new Error('Compressed download exceeds its byte limit');
    return new Uint8Array(await new Blob(await readLimited(response.body.getReader(), limit)).arrayBuffer());
}

const cachedPath = '.webfront/developer/environment.tgz';

async function verifiedCache(bridge, integrity) {
    try {
        const candidate = await readProject(bridge, cachedPath);
        await environmentFiles(candidate, integrity);
        return candidate;
    } catch { return null; }
}

async function downloadEnvironment(config, fetchImpl) {
    const url = new URL(config.environment, location.href);
    if (url.protocol !== 'https:' && url.origin !== location.origin)
        throw new Error('The environment must use HTTPS or the application origin');
    const response = await fetchImpl(url);
    if (!response.ok) throw new Error(`Environment download: HTTP ${response.status}`);
    return responseBytes(response);
}

function validStarter(starter) {
    if (!starter || typeof starter.archive !== 'string' || !safePath(starter.archive)) return false;
    return typeof starter.integrity === 'string' && starter.integrity.startsWith('sha512-');
}

function pinnedStarters(manifest, environmentUrl) {
    const starters = {};
    for (const [name, starter] of Object.entries(manifest.starters ?? {})) {
        if (!validStarter(starter)) throw new Error('Invalid pinned starter metadata');
        starters[name] = {url: new URL(starter.archive, environmentUrl).href, integrity: starter.integrity};
    }
    return starters;
}

export async function bootstrap(bridge, config, fetchImpl = fetch, progress = () => {}) {
    let bytes = await verifiedCache(bridge, config.integrity);
    if (bytes) progress('Using verified cached developer environment');
    else {
        progress('Downloading developer environment');
        bytes = await downloadEnvironment(config, fetchImpl);
    }
    const {files, manifest} = await environmentFiles(bytes, config.integrity);
    const starters = pinnedStarters(manifest, new URL(config.environment, location.href));
    // All files and the trusted archive are published only after validation succeeds.
    await publish(bridge, '.webfront/developer/environment', files);
    await publish(bridge, cachedPath, new Map([['output', bytes]]), {directory: false});
    return {url: `/webfront/developer/environment/${manifest.entry}`, config: {...config, starters}};
}
