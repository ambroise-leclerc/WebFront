import {gunzip, readTar, verifyIntegrity} from '../components/tools/wf.mjs';
import {readProject, publish, responseBytes} from './io.mjs';

const decoder = new TextDecoder();

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

function supported(manifest, files) {
    if (manifest.protocol !== 1) return false;
    if (!safePath(manifest.entry)) return false;
    return files.has(manifest.entry);
}

export async function environmentFiles(bytes, integrity) {
    await verifyIntegrity(bytes, integrity);
    const files = archiveFiles(await gunzip(bytes));
    if (!files.has('environment.json')) throw new Error('Environment archive has no environment.json');
    const manifest = JSON.parse(decoder.decode(files.get('environment.json')));
    if (!supported(manifest, files)) throw new Error('Unsupported WebFront developer environment');
    return {files, manifest};
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
    if (!starter) return false;
    if (!safePath(starter.archive)) return false;
    return String(starter.integrity).startsWith('sha512-');
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
