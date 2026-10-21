// npm library resolution for the browser. Tools continue to use the existing wf installer.
import semver from 'semver';
import {parseSpec} from '../components/tools/wf.mjs';
import {downloadPackage} from './registry.mjs';
import {validateLock} from './lock.mjs';

import {safePackagePath} from './paths.mjs';
export {safePackagePath, downloadPackage, validateLock};

// The installed version satisfying a range, so that a later install never moves or replaces it silently.
function lockedNode(previous, name, range) {
    if (!semver.validRange(range)) return null;
    const candidates = Object.values(previous?.packages ?? {}).filter(node => node.name === name && semver.satisfies(node.version, range));
    return candidates.sort((a, b) => semver.rcompare(a.version, b.version))[0] ?? null;
}

// Downloads each name@range once; versions already in options.previous are kept and their archives must
// still match the recorded integrity.
function pinnedDownloader({registry = 'https://registry.npmjs.org/', fetchImpl = fetch, signal, download = downloadPackage, previous}) {
    const requests = new Map();
    return async (name, range) => {
        const request = `${name}@${range}`;
        if (requests.has(request)) return requests.get(request);
        const pinned = lockedNode(previous, name, range);
        const pkg = await download(name, pinned?.version ?? range, {registry, fetchImpl, signal});
        if (pinned && pkg.integrity !== pinned.integrity)
            throw new Error(`${pkg.id} no longer matches the integrity recorded in the lock`);
        requests.set(request, pkg);
        return pkg;
    };
}

// Optional peers may be absent; others must be satisfied by a root or a direct dependency.
const peerError = (node, name, range) => new Error(`${node.name}@${node.version} requires peer ${name}@${range}; install a compatible root explicitly`);

function optionalPeer(node, name, range) {
    if (node.manifest.peerDependenciesMeta?.[name]?.optional) return null;
    throw peerError(node, name, range);
}

function peerOf(node, [name, range], {nodes, roots}) {
    const peer = roots[name]?.id ?? node.dependencies[name];
    if (!peer) return optionalPeer(node, name, range);
    if (!semver.satisfies(nodes[peer].version, range)) throw peerError(node, name, range);
    return peer;
}

function connectNode(node, graph) {
    for (const requirement of Object.entries(node.manifest.peerDependencies ?? {})) {
        const peer = peerOf(node, requirement, graph);
        if (peer) node.dependencies[requirement[0]] = peer;
    }
}

function connectPeers(nodes, roots) {
    for (const node of Object.values(nodes)) connectNode(node, {nodes, roots});
}

function addNode(state, name, pkg) {
    if (Object.keys(state.nodes).length >= 512) throw new Error('Dependency graph exceeds 512 packages');
    for (const bytes of pkg.files.values()) state.total += bytes.length;
    if (state.total > 256 * 1024 * 1024) throw new Error('Dependency graph exceeds 256 MiB');
    state.nodes[pkg.id] = {name, version: pkg.manifest.version, integrity: pkg.integrity, key: pkg.key,
        manifest: pkg.manifest, files: [...pkg.files.keys()].sort(), dependencies: {}};
    state.downloads.set(pkg.id, pkg.files);
}

async function resolveRoots(specifications, visit) {
    const roots = {};
    for (const spec of specifications) {
        const {name, version} = parseSpec(spec);
        roots[name] = {requested: version || 'latest', id: await visit(name, version || 'latest')};
    }
    return roots;
}

// A distinct node per exact version; edges are retained instead of flattening node_modules.
export async function resolvePackages(specifications, options = {}) {
    const fetchPackage = pinnedDownloader(options);
    const state = {nodes: {}, downloads: new Map(), total: 0};
    const visit = async (name, range) => {
        options.signal?.throwIfAborted();
        const pkg = await fetchPackage(name, range);
        if (state.nodes[pkg.id]) return pkg.id; // includes cycles currently being resolved
        addNode(state, name, pkg);
        for (const [dependency, version] of Object.entries(pkg.manifest.dependencies ?? {}).sort())
            state.nodes[pkg.id].dependencies[dependency] = await visit(dependency, version);
        // Optional dependencies are intentionally not installed in v1. An import of one fails clearly.
        return pkg.id;
    };
    const roots = await resolveRoots(specifications, visit);
    connectPeers(state.nodes, roots);
    return {lock: {format: 1, registry: options.registry ?? 'https://registry.npmjs.org/', roots, packages: state.nodes}, downloads: state.downloads};
}
