// wf restore: rebuilds the package cache from the lock alone, from one registry or, with --offline, from nothing.
import {downloadPackage, registryBase} from './registry.mjs';
import {inspectCache, storePackage} from './cache.mjs';
import {loadProject, syncNotice, lockPath, noManifest} from './project.mjs';
import {manifestPath} from './manifest.mjs';

export const defaultRegistry = 'https://registry.npmjs.org/';
/**
 * The registry the commands use, and where it comes from: the application's explicit option (the distributor's
 * channel, which also lets a test impose its local registry), then the manifest's "registry", then the default.
 */
export function effectiveRegistry({registry, registryExplicit} = {}, manifest) {
    if (registryExplicit && registry) return {url: registry, source: 'application option'};
    if (manifest?.registry) return {url: manifest.registry, source: manifestPath};
    return {url: registry || defaultRegistry, source: 'default'};
}

/** The effective registry of a command that may download, refused before any request when it is not allowed. */
export function usableRegistry(options, manifest) {
    const effective = effectiveRegistry(options, manifest);
    registryBase(effective.url);
    return effective;
}

export const describeRegistry = ({url, source}) => `Registry: ${url} (${source})`;

export const unpackedLimit = 256 * 1024 * 1024;
const unpackedSize = files => [...files.values()].reduce((total, bytes) => total + bytes.length, 0);
const tooLarge = limit => new Error(`The locked packages exceed ${limit / (1024 * 1024)} MiB unpacked`);

// Every package's cache state; the valid ones count towards the bound of the graph.
async function inspectAll(bridge, lock, {signal, limit}) {
    const states = [];
    let size = 0;
    for (const [id, node] of Object.entries(lock.packages)) {
        signal?.throwIfAborted();
        const inspected = await inspectCache(bridge, node, {signal, limit: limit - size});
        size += inspected.size;
        states.push({id, node, state: inspected.state});
    }
    return {states, size};
}

/**
 * Brings the cache to the exact locked graph: each package is checked against its record and, when missing or
 * corrupted, downloaded at its locked version (no range is resolved again) and checked against the lock's SHA-512.
 * The unpacked graph, cached packages included, is bounded by `limit`, checked before each publication, so a
 * restore that is resumed keeps the bound. Each package is published on its own: an interruption or an error keeps
 * every package already restored, and the lock is never written. Offline, nothing is downloaded: the packages that
 * would be are listed in an error.
 */
export async function restorePackages(bridge, lock, {registry, offline = false, signal, progress = () => {}, fetchImpl = fetch, limit = unpackedLimit}) {
    const {states, size: cached} = await inspectAll(bridge, lock, {signal, limit});
    let size = cached;
    const result = {verified: states.filter(({state}) => state === 'valid').length, restored: 0, problems: []};
    for (const {id, node, state} of states.filter(({state}) => state !== 'valid')) {
        if (offline) { result.problems.push(`${id} (${state})`); continue; }
        progress(`Restoring ${id}${state === 'corrupted' ? ' (corrupted in the cache)' : ''}`);
        const pkg = await downloadPackage(node.name, node.version, {registry, fetchImpl, signal});
        if (pkg.integrity !== node.integrity) throw new Error(`${id} no longer matches the integrity recorded in the lock`);
        size += unpackedSize(pkg.files);
        if (size > limit) throw tooLarge(limit);
        await storePackage(bridge, node, pkg.files, {signal});
        result.restored++;
    }
    if (result.problems.length)
        throw new Error(`The cache does not match ${lockPath}; missing or corrupted, nothing downloaded:\r\n  ${result.problems.join('\r\n  ')}\r\nRun wf restore to download them.`);
    return result;
}

function parseOptions(args) {
    const offline = args.includes('--offline');
    if (args.some(arg => arg !== '--offline')) throw new Error('Usage: wf restore [--offline]');
    return {offline};
}

/** wf restore [--offline]: the cache is rebuilt from the lock; manifest and lock are only read. */
export async function restore(bridge, args, {registry, registryExplicit, signal, progress = () => {}, fetchImpl = fetch} = {}) {
    const {offline} = parseOptions(args);
    const project = await loadProject(bridge);
    if (!project.manifest) throw new Error(noManifest);
    if (!project.lock) throw new Error(`No ${lockPath}: run wf install to resolve the dependencies`);
    const effective = offline ? null : usableRegistry({registry, registryExplicit}, project.manifest);
    progress(offline ? 'Offline: no registry is contacted' : describeRegistry(effective));
    const result = await restorePackages(bridge, project.lock, {registry: effective?.url, offline, signal, progress, fetchImpl});
    const count = Object.keys(project.lock.packages).length;
    const summary = offline
        ? `Cache verified offline: ${count} packages match ${lockPath}.`
        : `Restored ${result.restored} of ${count} packages (${result.verified} already in the cache). Lock: ${lockPath}`;
    return {...result, registry: effective, text: syncNotice(project) + summary};
}
