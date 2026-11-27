// The installed packages as a graph, read from the lock alone: wf list --tree, wf why, wf info and the Packages panel of the
// inspectors (inspector-packages.mjs). The lock records, for every package, the exact package each of its dependencies and
// satisfied peers resolved to.
import {packageRoot} from './paths.mjs';
import {inline} from './terminal.mjs';

const byName = (a, b) => a < b ? -1 : a > b ? 1 : 0;
// Everything shown comes from the lock, so from the package.json of packages: control characters are made visible.
const idOf = node => inline(`${node.name}@${node.version}`);

// How a package asks for one of its dependencies: the range of its package.json and the kind of the dependency.
function asked(node, name) {
    const manifest = node.manifest ?? {};
    if (manifest.dependencies?.[name] !== undefined) return {kind: 'dependency', range: inline(manifest.dependencies[name])};
    if (manifest.optionalDependencies?.[name] !== undefined) return {kind: 'optional', range: inline(manifest.optionalDependencies[name])};
    if (manifest.peerDependencies?.[name] !== undefined) return {kind: 'peer', range: inline(manifest.peerDependencies[name])};
    return {kind: 'dependency', range: ''};
}
// The same as wf writes it: the range, marked when it is optional or a peer.
const written = ({kind, range}) => kind === 'dependency' ? range : `${kind} ${range}`;
const request = (node, name) => written(asked(node, name));
const requested = range => range ? `  (${inline(range)})` : '';
const edges = node => Object.entries(node.dependencies ?? {}).sort(([a], [b]) => byName(a, b));

/**
 * The locked graph as a tree under the roots of the manifest, for wf list --tree and for the Packages panel of the
 * inspectors (#683). A package is opened once: where it comes again, `deduped` stands for the dependencies already shown
 * and `children` is empty. Each node is {id, name, version, label, kind, range, requested, deduped, versions, children}:
 * `kind` is 'root', 'dependency', 'optional' or 'peer', `range` what asks for it, `requested` the two as wf writes them,
 * `versions` how many versions of that name are locked. `label` and the ranges have their control characters made visible.
 */
export function dependencyGraph(lock) {
    const packages = lock?.packages ?? {};
    const versions = new Map();
    for (const node of Object.values(packages)) versions.set(node.name, (versions.get(node.name) ?? 0) + 1);
    const opened = new Set();
    const open = (id, kind, range, requested) => {
        const node = packages[id];
        const deduped = opened.has(id) && edges(node).length > 0;
        const entry = {id, name: node.name, version: node.version, label: idOf(node), kind, range, requested, deduped,
            versions: versions.get(node.name), children: []};
        if (opened.has(id)) return entry;
        opened.add(id);
        for (const [name, child] of edges(node)) {
            const how = asked(node, name);
            entry.children.push(open(child, how.kind, how.range, written(how)));
        }
        return entry;
    };
    const roots = Object.entries(lock?.roots ?? {}).sort(([a], [b]) => byName(a, b))
        .map(([, root]) => {
            const range = root.requested ? inline(root.requested) : '';
            return open(root.id, 'root', range, range);
        });
    return {roots, packages: Object.keys(packages).length};
}

/** wf list --tree: the tree of dependencyGraph() as text. */
export function dependencyTree(lock) {
    const graph = dependencyGraph(lock);
    if (!graph.roots.length) return 'No installed packages.';
    const lines = [];
    const line = entry => `${entry.label}${requested(entry.requested)}${entry.deduped ? '  deduped' : ''}`;
    const children = (entry, prefix) => entry.children.forEach((child, index) => {
        const last = index === entry.children.length - 1;
        lines.push(`${prefix}${last ? '└── ' : '├── '}${line(child)}`);
        children(child, prefix + (last ? '    ' : '│   '));
    });
    for (const root of graph.roots) {
        lines.push(line(root));
        children(root, '');
    }
    const count = graph.packages;
    lines.push('', `${graph.roots.length} ${graph.roots.length === 1 ? 'root' : 'roots'}, ${count} ${count === 1 ? 'package' : 'packages'}`);
    return lines.join('\n');
}

/** The locked packages a word designates: every version of a name, or the one "name@version". Throws when none is. */
export function selectPackages(lock, spec) {
    const found = Object.entries(lock?.packages ?? {}).filter(([, node]) => node.name === spec || `${node.name}@${node.version}` === spec)
        .sort(([, a], [, b]) => byName(idOf(a), idOf(b)));
    if (!found.length) throw new Error(`${inline(spec)} is not installed (wf list names the installed packages)`);
    return found;
}

const rootsOf = (lock, id) => Object.entries(lock.roots ?? {}).filter(([, root]) => root.id === id);
const dependents = (lock, id) => Object.entries(lock.packages)
    .flatMap(([parent, node]) => edges(node).filter(([, child]) => child === id).map(([name]) => ({parent, node, name})))
    .sort((a, b) => byName(idOf(a.node), idOf(b.node)));

export const chainLimit = 20;

/**
 * The chains of dependents from a locked package up to the roots, each from its root down, `chainLimit` at most (`limited`
 * says that there may be more). A link is {id, label, note}: `note` is how the package above asks for it, or for a root
 * "root" and the range of the manifest.
 */
export function whyChains(lock, target) {
    const found = [];
    const link = (id, note) => ({id, label: idOf(lock.packages[id]), note});
    const climb = (id, below, seen) => {
        if (found.length >= chainLimit) return;
        for (const [, root] of rootsOf(lock, id))
            found.push([link(id, `root${root.requested ? `, ${inline(root.requested)}` : ''}`), ...below]);
        for (const {parent, node, name} of dependents(lock, id)) {
            if (seen.has(parent)) continue;  // a cycle of peers: the chain through it was already taken
            climb(parent, [link(id, request(node, name)), ...below], new Set([...seen, parent]));
        }
    };
    climb(target, [], new Set([target]));
    const chains = found.slice(0, chainLimit);
    return {chains, limited: chains.length === chainLimit};
}

/** wf why <package>: why each locked version of it is installed, as chains from the roots of the manifest. */
export function why(lock, spec) {
    return selectPackages(lock, spec).map(([id, node]) => {
        const {chains, limited} = whyChains(lock, id);
        const lines = chains.map(chain => `  ${chain.map(({label, note}) => `${label}${note ? ` (${note})` : ''}`).join(' > ')}`);
        if (!chains.length) lines.push('  nothing requires it: wf install --update resolves the manifest again');
        if (limited) lines.push(`  ... only the first ${chainLimit} chains are shown`);
        return [idOf(node), ...lines].join('\n');
    }).join('\n');
}

// "license" is a string; older packages have {type} or a "licenses" array of them. Anything else is not a declaration.
function license(manifest) {
    const names = [manifest?.license, manifest?.licenses].flat()
        .map(value => typeof value === 'string' ? value : value?.type).filter(value => typeof value === 'string' && value);
    return names.length ? inline(names.join(', ')) : 'not declared';
}
function executables(node) {
    const bin = node.manifest?.webfront?.bin;
    if (typeof bin === 'string') return [inline(node.name.split('/').pop())];
    return bin && typeof bin === 'object' && !Array.isArray(bin) ? Object.keys(bin).map(inline) : [];
}

/**
 * What the lock records of one locked package, for wf info and for the Packages panel: {id, name, version, label, roots (the
 * ranges of the manifest that ask for it), description, license, integrity, files, directory, dependencies ({id, label,
 * request}), dependents ({id, label}), executables ({name, command})}. Every text has its control characters made visible.
 */
export function packageDetails(lock, id) {
    const node = lock.packages[id];
    const bins = executables(node);
    const neededBy = new Map(dependents(lock, id).map(({parent, node: above}) => [parent, {id: parent, label: idOf(above)}]));
    return {
        id, name: inline(node.name), version: inline(node.version), label: idOf(node),
        roots: rootsOf(lock, id).map(([, root]) => inline(root.requested)),
        description: typeof node.manifest?.description === 'string' ? inline(node.manifest.description) : '',
        license: license(node.manifest),
        integrity: inline(node.integrity),
        files: node.files.map(inline),
        directory: `${packageRoot}/${inline(node.key)}/`,
        dependencies: edges(node).map(([name, child]) => ({id: child, label: idOf(lock.packages[child]), request: request(node, name)})),
        dependents: [...neededBy.values()],
        executables: bins.map(name => ({name, command: `wf exec ${inline(node.name)}${bins.length > 1 ? `:${name}` : ''}`})),
    };
}

/** wf info <package>: what the lock records of each locked version of it. */
export function info(lock, spec) {
    return selectPackages(lock, spec).map(([id]) => {
        const details = packageDetails(lock, id);
        const needs = details.dependencies.map(({label, request: how}) => `${label}${how ? ` (${how})` : ''}`);
        const neededBy = [...new Set(details.dependents.map(({label}) => label))];
        const rows = [
            ['root', details.roots.length ? `yes, requested ${details.roots.join(', ')}` : 'no, installed for another package (wf why)'],
            ...(details.description ? [['description', details.description]] : []),
            ['license', details.license],
            ['integrity', details.integrity],
            ['files', `${details.files.length} in ${details.directory}`],
            ['dependencies', needs.join(', ') || 'none'],
            ['dependents', neededBy.join(', ') || 'none'],
            ...(details.executables.length ? [['executables', details.executables.map(({command}) => command).join(', ')]] : []),
        ];
        return [details.label, ...rows.map(([label, value]) => `  ${label.padEnd(12)}  ${value}`)].join('\n');
    }).join('\n');
}
