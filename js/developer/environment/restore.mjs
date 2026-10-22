// wf restore: rebuilds the package cache from the lock alone, from one registry or, with --offline, from nothing.
import {downloadPackage, registryBase} from './registry.mjs';
import {cacheState, storePackage} from './cache.mjs';
import {loadProject, syncNotice, lockPath, noManifest} from './project.mjs';
import {manifestPath} from './manifest.mjs';

export const defaultRegistry = 'https://registry.npmjs.org/';
const unpackedLimit = 256 * 1024 * 1024;

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

const unpackedSize = files => [...files.values()].reduce((total, bytes) => total + bytes.length, 0);

async function restorePackage(bridge, [id, node], {registry, fetchImpl, signal}) {
    const pkg = await downloadPackage(node.name, node.version, {registry, fetchImpl, signal});
    if (pkg.integrity !== node.integrity) throw new Error(`${id} no longer matches the integrity recorded in the lock`);
    await storePackage(bridge, node, pkg.files, {signal});
    return unpackedSize(pkg.files);
}

/**
 * Brings the cache to the exact locked graph: each package is checked against its record and, when missing or
 * corrupted, downloaded at its locked version (no range is resolved again) and checked against the lock's SHA-512.
 * Each package is published on its own, so an interruption or an error keeps every package already restored, and
 * the lock is never written. Offline, nothing is downloaded: the packages that would be are listed in an error.
 */
export async function restorePackages(bridge, lock, {registry, offline = false, signal, progress = () => {}, fetchImpl = fetch}) {
    const result = {verified: 0, restored: 0, problems: []};
    let unpacked = 0;
    for (const entry of Object.entries(lock.packages)) {
        signal?.throwIfAborted();
        const state = await cacheState(bridge, entry[1], {signal});
        if (state === 'valid') { result.verified++; continue; }
        if (offline) { result.problems.push(`${entry[0]} (${state})`); continue; }
        progress(`Restoring ${entry[0]}${state === 'corrupted' ? ' (corrupted in the cache)' : ''}`);
        unpacked += await restorePackage(bridge, entry, {registry, fetchImpl, signal});
        if (unpacked > unpackedLimit) throw new Error('Restored packages exceed 256 MiB');
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
