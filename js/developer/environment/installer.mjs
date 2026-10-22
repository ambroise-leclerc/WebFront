// wf init and wf install: the manifest states the intent, the lock the exact resolution; both are published together.
import {resolvePackages, downloadPackage} from './packages.mjs';
import {publish, readProject} from './io.mjs';
import {safePackagePath} from './paths.mjs';
import {manifestPath, newManifest, manifestName, serializeManifest, dependenciesDigest} from './manifest.mjs';
import {loadProject, publishTogether, lockPath, packageRoot, noManifest, desynchronized, textBytes} from './project.mjs';

const splitSpec = spec => {
    const at = spec.indexOf('@', spec.startsWith('@') ? 1 : 0);
    return at < 0 ? [spec, undefined] : [spec.slice(0, at), spec.slice(at + 1) || undefined];
};

/** Creates a minimal manifest without replacing an existing one. */
export async function initProject(bridge, [directory = '.', ...extra], {signal} = {}) {
    if (extra.length) throw new Error('Usage: wf init [directory]');
    const folder = directory === '.' ? '' : safePackagePath(directory.replace(/\/+$/, ''));
    const path = folder ? `${folder}/${manifestPath}` : manifestPath;
    const files = new Map([['output', textBytes(serializeManifest(newManifest(manifestName(folder))))]]);
    await publish(bridge, path, files, {directory: false, replace: false, signal});
    return `Created ${path}. Add libraries with wf install <package>.`;
}

// The ranges to resolve: the manifest's, with the requested packages added or replaced.
function requestedDependencies(manifest, specs) {
    const dependencies = {...manifest.dependencies};
    const explicit = new Set();
    for (const spec of specs) {
        const [name, range] = splitSpec(spec);
        dependencies[name] = range ?? 'latest';
        if (range && range !== 'latest') explicit.add(name);
    }
    return {dependencies, explicit};
}

// A package installed without a range is recorded as ^version, like npm; an explicit range is kept as typed.
function recordedDependencies({dependencies, explicit}, lock, specs) {
    const recorded = {...dependencies};
    for (const spec of specs) {
        const name = splitSpec(spec)[0];
        if (!explicit.has(name)) recorded[name] = `^${lock.packages[lock.roots[name].id].version}`;
    }
    return recorded;
}

async function resolveLock(dependencies, previous, options) {
    const roots = Object.entries(dependencies).map(([name, range]) => `${name}@${range}`);
    const {lock, downloads} = await resolvePackages(roots, {
        registry: options.registry, signal: options.signal, fetchImpl: options.fetchImpl, previous,
        download: async (...args) => { options.progress(`Resolving ${args[0]}@${args[1]}`); return downloadPackage(...args); },
    });
    return {lock, downloads};
}

async function storePackages(bridge, lock, downloads, {signal, progress}) {
    for (const [id, files] of downloads) {
        progress(`Storing ${id}`);
        await publish(bridge, `${packageRoot}/${lock.packages[id].key}`, files, {signal});
    }
}

async function isCached(bridge, node) {
    try { await readProject(bridge, `${packageRoot}/${node.key}/package.json`); return true; }
    catch { return false; }
}

// npm ci: exactly the locked graph, every archive checked against the recorded integrity. Nothing is resolved or written
// besides the cache.
async function restoreLocked(bridge, lock, {signal, progress, fetchImpl}) {
    let restored = 0;
    for (const [id, node] of Object.entries(lock.packages)) {
        signal?.throwIfAborted();
        if (await isCached(bridge, node)) continue;
        progress(`Restoring ${id}`);
        const pkg = await downloadPackage(node.name, node.version, {registry: lock.registry, fetchImpl, signal});
        if (pkg.integrity !== node.integrity) throw new Error(`${id} no longer matches the integrity recorded in the lock`);
        await publish(bridge, `${packageRoot}/${node.key}`, pkg.files, {signal});
        restored++;
    }
    return restored;
}

function parseOptions(args) {
    const update = args.includes('--update');
    const specs = args.filter(arg => arg !== '--update');
    if (specs.some(spec => spec.startsWith('-'))) throw new Error('Usage: wf install [--update] [<package>[@range] ...]');
    return {update, specs};
}

async function publishResolution(bridge, project, {manifest, lock}, options) {
    const files = new Map([[lockPath, textBytes(JSON.stringify(lock, null, 2) + '\n')]]);
    if (manifest) files.set(manifestPath, textBytes(serializeManifest(manifest)));
    await publishTogether(bridge, files, project, options);
}

const withDigest = async (lock, dependencies) => ({format: lock.format, manifestDigest: await dependenciesDigest(dependencies), ...lock});

/**
 * wf install <pkg>[@range]: resolve, download and verify, then publish manifest and lock in one change.
 * wf install: the locked graph exactly when the lock agrees with the manifest; resolves it when there is no lock.
 * wf install --update: resolves the manifest again, keeping nothing from the lock.
 */
export async function install(bridge, args, {registry, signal, progress = () => {}, fetchImpl = fetch}) {
    const {update, specs} = parseOptions(args);
    const project = await loadProject(bridge);
    if (!project.manifest) throw new Error(noManifest);
    const options = {registry: project.manifest.registry ?? registry, signal, fetchImpl, progress};
    if (specs.length && update) throw new Error('Usage: wf install --update does not take packages');
    if (specs.length && project.status === 'desynchronized') throw new Error(desynchronized);
    if (!specs.length && !update && project.status === 'desynchronized') throw new Error(desynchronized);
    if (!specs.length && !update && project.status === 'consistent')
        return {lock: project.lock, restored: await restoreLocked(bridge, project.lock, {signal, progress, fetchImpl: options.fetchImpl})};
    const request = requestedDependencies(project.manifest, specs);
    // --update ignores the locked versions; installing a package keeps the others where they are.
    const previous = project.status === 'consistent' && !update ? project.lock : undefined;
    const resolved = await resolveLock(request.dependencies, previous, options);
    const dependencies = recordedDependencies(request, resolved.lock, specs);
    await storePackages(bridge, resolved.lock, resolved.downloads, {signal, progress});
    // The journal of the joint publication is the installation commit point. A failed install may leave unused cache files.
    const lock = await withDigest(resolved.lock, dependencies);
    const manifest = specs.length ? {...project.manifest, dependencies} : null;
    await publishResolution(bridge, project, {manifest, lock}, {signal});
    return {lock, restored: 0};
}
