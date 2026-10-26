// wf init and wf install: the manifest states the intent, the lock the exact resolution; both are published together.
import semver from 'semver';
import {resolvePackages, downloadPackage} from './packages.mjs';
import {parseSpec} from './tarball.mjs';
import {publish} from './io.mjs';
import {manifestPath, newManifest, manifestName, serializeManifest, validateManifest, dependenciesDigest} from './manifest.mjs';
import {loadProject, publishTogether, lockPath, noManifest, desynchronized, textBytes} from './project.mjs';
import {storePackage} from './cache.mjs';
import {restorePackages, usableRegistry, describeRegistry} from './restore.mjs';

// The spec syntax resolvePackages() reads: a missing version is undefined, resolved as latest.
const splitSpec = spec => {
    const {name, version} = parseSpec(spec);
    return [name, version || undefined];
};

// The range a manifest records for a root of a lock written before the manifest: the range it was installed with,
// or ^version for an exact version or a dist-tag, like npm.
function legacyRange(lock, root) {
    const {version} = lock.packages[root.id];
    const exact = semver.valid(root.requested) !== null;
    return !exact && root.requested !== 'latest' && semver.validRange(root.requested) !== null ? root.requested : `^${version}`;
}

/**
 * Creates webfront.project.json at the project root, without replacing an existing one. Every wf command reads that
 * manifest. An existing lock gives the dependencies, so a project installed before the manifest keeps its locked graph.
 */
export async function initProject(bridge, [name, ...extra], {signal} = {}) {
    if (extra.length) throw new Error('Usage: wf init [name]');
    const {lock} = await loadProject(bridge);
    const manifest = newManifest(name ?? manifestName(''));
    if (lock) manifest.dependencies = Object.fromEntries(Object.entries(lock.roots).map(([root, entry]) => [root, legacyRange(lock, entry)]));
    validateManifest(manifest);
    await publish(bridge, manifestPath, new Map([['output', textBytes(serializeManifest(manifest))]]), {directory: false, replace: false, signal});
    const adopted = lock ? ` with the ${Object.keys(manifest.dependencies).length} dependencies of ${lockPath}` : '';
    return `Created ${manifestPath}${adopted}. Add libraries with wf install <package>.`;
}

// The ranges to resolve: the manifest's, with the requested packages added or replaced. They are checked like the manifest
// that will record them before anything is resolved, so a range the manifest cannot hold is refused instead of written.
function requestedDependencies(project, specs) {
    const dependencies = {...project.manifest.dependencies};
    const explicit = new Set();
    for (const spec of specs) {
        const [name, range] = splitSpec(spec);
        dependencies[name] = range ?? 'latest';
        if (range && range !== 'latest') explicit.add(name);
    }
    validateManifest({...project.source, dependencies});
    return {dependencies, explicit};
}

// The manifest as written with new dependencies: the defaults added by validation are not written back.
function updatedManifest(project, dependencies) {
    const manifest = {...project.source, dependencies};
    validateManifest(manifest);
    return manifest;
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
        await storePackage(bridge, lock.packages[id], files, {signal});
    }
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
export async function install(bridge, args, {registry, registryExplicit, signal, progress = () => {}, fetchImpl = fetch}) {
    const {update, specs} = parseOptions(args);
    const project = await loadProject(bridge);
    if (!project.manifest) throw new Error(noManifest);
    const effective = usableRegistry({registry, registryExplicit}, project.manifest);
    const options = {registry: effective.url, signal, fetchImpl, progress};
    if (specs.length && update) throw new Error('Usage: wf install --update does not take packages');
    if (specs.length && project.status === 'desynchronized') throw new Error(desynchronized);
    if (!specs.length && !update && project.status === 'desynchronized') throw new Error(desynchronized);
    progress(describeRegistry(effective));
    // Exactly the locked graph, every archive checked against the recorded integrity.
    const locked = project.status === 'consistent' || project.status === 'legacy';
    if (!specs.length && !update && locked) {
        const {restored} = await restorePackages(bridge, project.lock, options);
        if (project.status === 'consistent') return {lock: project.lock, restored, registry: effective};
        // A lock written before the manifest: the same graph, now recording the dependencies it was resolved from.
        const lock = await withDigest(project.lock, project.manifest.dependencies);
        await publishResolution(bridge, project, {manifest: null, lock}, {signal});
        return {lock, restored, registry: effective};
    }
    const request = requestedDependencies(project, specs);
    // --update ignores the locked versions; installing a package keeps the others where they are.
    const previous = locked && !update ? project.lock : undefined;
    const resolved = await resolveLock(request.dependencies, previous, options);
    const dependencies = recordedDependencies(request, resolved.lock, specs);
    await storePackages(bridge, resolved.lock, resolved.downloads, {signal, progress});
    // The journal of the joint publication is the installation commit point. A failed install may leave unused cache files.
    const lock = await withDigest(resolved.lock, dependencies);
    const manifest = specs.length ? updatedManifest(project, dependencies) : null;
    await publishResolution(bridge, project, {manifest, lock}, {signal});
    return {lock, restored: 0, registry: effective};
}
