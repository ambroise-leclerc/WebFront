// The installed packages as a graph, read from the lock alone: wf list --tree, wf why and wf info. The lock records, for
// every package, the exact package each of its dependencies and satisfied peers resolved to.
import {packageRoot} from './paths.mjs';

const byName = (a, b) => a < b ? -1 : a > b ? 1 : 0;
const idOf = node => `${node.name}@${node.version}`;

// How a package asks for one of its dependencies: the range of its package.json, marked when it is a peer.
function request(node, name) {
    const manifest = node.manifest ?? {};
    if (manifest.dependencies?.[name] !== undefined) return manifest.dependencies[name];
    if (manifest.optionalDependencies?.[name] !== undefined) return `optional ${manifest.optionalDependencies[name]}`;
    if (manifest.peerDependencies?.[name] !== undefined) return `peer ${manifest.peerDependencies[name]}`;
    return '';
}
const requested = range => range ? `  (${range})` : '';
const edges = node => Object.entries(node.dependencies ?? {}).sort(([a], [b]) => byName(a, b));

/**
 * The locked graph as a tree under the roots of the manifest, each with the range it was requested with. A package
 * is opened once: where it comes again, "deduped" stands for the dependencies already shown.
 */
export function dependencyTree(lock) {
    const roots = Object.entries(lock?.roots ?? {}).sort(([a], [b]) => byName(a, b));
    if (!roots.length) return 'No installed packages.';
    const lines = [], opened = new Set();
    const children = (id, prefix) => {
        const node = lock.packages[id];
        const list = edges(node);
        list.forEach(([name, child], index) => {
            const last = index === list.length - 1;
            const childNode = lock.packages[child];
            const again = opened.has(child) && edges(childNode).length > 0;
            lines.push(`${prefix}${last ? '└── ' : '├── '}${idOf(childNode)}${requested(request(node, name))}${again ? '  deduped' : ''}`);
            if (opened.has(child)) return;
            opened.add(child);
            children(child, prefix + (last ? '    ' : '│   '));
        });
    };
    for (const [, root] of roots) {
        const again = opened.has(root.id) && edges(lock.packages[root.id]).length > 0;
        lines.push(`${idOf(lock.packages[root.id])}${requested(root.requested)}${again ? '  deduped' : ''}`);
        if (opened.has(root.id)) continue;
        opened.add(root.id);
        children(root.id, '');
    }
    const count = Object.keys(lock.packages).length;
    lines.push('', `${roots.length} ${roots.length === 1 ? 'root' : 'roots'}, ${count} ${count === 1 ? 'package' : 'packages'}`);
    return lines.join('\n');
}

/** The locked packages a word designates: every version of a name, or the one "name@version". Throws when none is. */
export function selectPackages(lock, spec) {
    const found = Object.entries(lock?.packages ?? {}).filter(([, node]) => node.name === spec || idOf(node) === spec)
        .sort(([, a], [, b]) => byName(idOf(a), idOf(b)));
    if (!found.length) throw new Error(`${spec} is not installed (wf list names the installed packages)`);
    return found;
}

const rootsOf = (lock, id) => Object.entries(lock.roots ?? {}).filter(([, root]) => root.id === id);
const dependents = (lock, id) => Object.entries(lock.packages)
    .flatMap(([parent, node]) => edges(node).filter(([, child]) => child === id).map(([name]) => ({parent, node, name})))
    .sort((a, b) => byName(idOf(a.node), idOf(b.node)));

export const chainLimit = 20;

// The chains of dependents from a package up to the roots, each written from its root down.
function chains(lock, target) {
    const found = [];
    const climb = (id, below, seen) => {
        if (found.length >= chainLimit) return;
        for (const [, root] of rootsOf(lock, id))
            found.push([`${idOf(lock.packages[id])} (root${root.requested ? `, ${root.requested}` : ''})`, ...below]);
        for (const {parent, node, name} of dependents(lock, id)) {
            if (seen.has(parent)) continue;  // a cycle of peers: the chain through it was already taken
            climb(parent, [`${idOf(lock.packages[id])}${request(node, name) ? ` (${request(node, name)})` : ''}`, ...below], new Set([...seen, parent]));
        }
    };
    climb(target, [], new Set([target]));
    return found.slice(0, chainLimit);
}

/** wf why <package>: why each locked version of it is installed, as chains from the roots of the manifest. */
export function why(lock, spec) {
    return selectPackages(lock, spec).map(([id, node]) => {
        const found = chains(lock, id);
        const lines = found.map(chain => `  ${chain.join(' > ')}`);
        if (!found.length) lines.push('  nothing requires it: wf install --update resolves the manifest again');
        if (found.length === chainLimit) lines.push(`  ... only the first ${chainLimit} chains are shown`);
        return [idOf(node), ...lines].join('\n');
    }).join('\n');
}

const license = manifest => typeof manifest?.license === 'string' ? manifest.license : manifest?.license?.type ?? 'not declared';
function executables(node) {
    const bin = node.manifest?.webfront?.bin;
    if (typeof bin === 'string') return [node.name.split('/').pop()];
    return bin && typeof bin === 'object' && !Array.isArray(bin) ? Object.keys(bin) : [];
}

/** wf info <package>: what the lock records of each locked version of it. */
export function info(lock, spec) {
    return selectPackages(lock, spec).map(([id, node]) => {
        const roots = rootsOf(lock, id);
        const needs = edges(node).map(([name, child]) => `${idOf(lock.packages[child])}${request(node, name) ? ` (${request(node, name)})` : ''}`);
        const neededBy = [...new Set(dependents(lock, id).map(({node: parent}) => idOf(parent)))];
        const bins = executables(node);
        const rows = [
            ['root', roots.length ? `yes, requested ${roots.map(([, root]) => root.requested).join(', ')}` : 'no, installed for another package (wf why)'],
            ...(node.manifest?.description ? [['description', node.manifest.description]] : []),
            ['license', license(node.manifest)],
            ['integrity', node.integrity],
            ['files', `${node.files.length} in ${packageRoot}/${node.key}/`],
            ['dependencies', needs.join(', ') || 'none'],
            ['dependents', neededBy.join(', ') || 'none'],
            ...(bins.length ? [['executables', bins.map(name => `wf exec ${node.name}${bins.length > 1 ? `:${name}` : ''}`).join(', ')]] : []),
        ];
        return [idOf(node), ...rows.map(([label, value]) => `  ${label.padEnd(12)}  ${value}`)].join('\n');
    }).join('\n');
}
