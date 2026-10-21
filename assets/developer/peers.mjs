// Peer dependencies: satisfied by an explicit root or a direct dependency, unless optional.
import semver from 'semver';

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

export function connectPeers(nodes, roots) {
    for (const node of Object.values(nodes)) connectNode(node, {nodes, roots});
}
