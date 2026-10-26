import {gunzip, readTar, verifyIntegrity} from './tarball.mjs';
import {readProject, publish, responseBytes} from './io.mjs';
import {isUnsafe} from './paths.mjs';

const safePath = path => !isUnsafe(path);

const decoder = new TextDecoder();

function addEntry(files, entry) {
    if (!entry.path.startsWith('package/')) throw new Error('Invalid environment archive root');
    const path = entry.path.slice(8);
    if (!safePath(path) || files.has(path)) throw new Error('Invalid or duplicate environment path');
    files.set(path, entry.bytes);
}

function archiveFiles(tarball) {
    const files = new Map();
    for (const entry of readTar(tarball)) addEntry(files, entry);
    return files;
}

// What the current C++ accepts when it does not say (a test, a config written before the contract): the first release.
const defaultSupported = {protocol: [1], api: [1]};

// An archive is refused with the reason, never used with a fallback. An environment that predates "api" was built for the
// first webfront.developer.* API, so a missing "api" reads as 1; any other declared version must be one the C++ supports.
function checkCompatible(manifest, supported) {
    const versions = {protocol: manifest.protocol, api: manifest.api ?? 1};
    for (const [name, version] of Object.entries(versions)) {
        const accepted = supported?.[name] ?? defaultSupported[name];
        if (!accepted.includes(version))
            throw new Error(`Incompatible WebFront developer environment: ${name} version ${version}, this application supports ${accepted.join(', ')}`);
    }
}

function checkEntry(manifest, files) {
    if (typeof manifest.entry !== 'string' || !safePath(manifest.entry) || !files.has(manifest.entry))
        throw new Error('Unsupported WebFront developer environment: invalid entry');
}

export async function environmentFiles(bytes, integrity, supported = defaultSupported) {
    await verifyIntegrity(bytes, integrity);
    const files = archiveFiles(await gunzip(bytes));
    if (!files.has('environment.json')) throw new Error('Environment archive has no environment.json');
    const manifest = JSON.parse(decoder.decode(files.get('environment.json')));
    checkCompatible(manifest, supported);
    checkEntry(manifest, files);
    return {files, manifest};
}

const cachedPath = '.webfront/developer/environment.tgz';

async function verifiedCache(bridge, integrity, supported) {
    try {
        const candidate = await readProject(bridge, cachedPath);
        await environmentFiles(candidate, integrity, supported);
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
    return Object.fromEntries(Object.entries(manifest.starters ?? {}).map(([name, starter]) => {
        if (!validStarter(starter)) throw new Error('Invalid pinned starter metadata');
        return [name, {url: new URL(starter.archive, environmentUrl).href, integrity: starter.integrity}];
    }));
}

export async function bootstrap(bridge, config, fetchImpl = fetch, progress = () => {}) {
    let bytes = await verifiedCache(bridge, config.integrity, config.supported);
    if (bytes) progress('Using verified cached developer environment');
    else {
        progress('Downloading developer environment');
        bytes = await downloadEnvironment(config, fetchImpl);
    }
    const {files, manifest} = await environmentFiles(bytes, config.integrity, config.supported);
    const starters = pinnedStarters(manifest, new URL(config.environment, location.href));
    // All files and the trusted archive are published only after validation succeeds.
    await publish(bridge, '.webfront/developer/environment', files);
    await publish(bridge, cachedPath, new Map([['output', bytes]]), {directory: false});
    return {url: `/webfront/developer/environment/${manifest.entry}`, config: {...config, starters}};
}
