// npm library resolution for the browser. Tools continue to use the existing wf installer.
import {parseSpec} from '../components/tools/wf.mjs';
import {downloadPackage} from './registry.mjs';
import {pinnedDownloader} from './pins.mjs';
import {connectPeers} from './peers.mjs';
import {validateLock} from './lock.mjs';

import {safePackagePath} from './paths.mjs';
export {safePackagePath, downloadPackage, validateLock};

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
