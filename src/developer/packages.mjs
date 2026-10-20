// npm library resolution for the browser. Tools continue to use the existing wf installer.
import semver from 'semver';
import {responseBytes} from './bootstrap.mjs';
import {gunzip, readTar, verifyIntegrity, parseSpec} from '../components/tools/wf.mjs';

import {safePackagePath} from './paths.mjs';
export {safePackagePath};

const text = new TextDecoder();
const safeName = name => /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/.test(name);
const idOf = (name, version) => `${name.replace('/', '+')}@${version}`;
const cacheKey = (name, version, integrity) => `${idOf(name, version)}-${integrity.slice(7).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '')}`;
const installScripts = ['preinstall', 'install', 'postinstall'];

async function fetchPackument(name, base, {fetchImpl, signal}) {
    const response = await fetchImpl(new URL(name.replace('/', '%2f'), base), {signal});
    if (!response.ok) throw new Error(`Registry metadata for ${name}: HTTP ${response.status}`);
    return response.json();
}

function resolveVersion(packument, spec) {
    const tagged = packument['dist-tags']?.[spec];
    if (tagged) return tagged;
    return semver.valid(spec) ? spec : semver.maxSatisfying(Object.keys(packument.versions ?? {}), spec);
}

function selectVersion(packument, name, requested) {
    const spec = requested || 'latest';
    const version = resolveVersion(packument, spec);
    const manifest = packument.versions?.[version];
    if (!manifest || !semver.valid(version)) throw new Error(`Cannot resolve ${name}@${spec}`);
    return {version, manifest};
}

async function readArchive(bytes) {
    const files = new Map();
    for (const entry of readTar(await gunzip(bytes))) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid npm tarball root');
        const path = safePackagePath(entry.path.slice(8));
        if (files.has(path)) throw new Error(`Duplicate package path: ${path}`);
        files.set(path, entry.bytes);
    }
    return files;
}

function needsInstallStep(pkg, files) {
    const scripts = pkg.scripts ?? {};
    if (installScripts.some(key => scripts[key]) || pkg.gypfile) return true;
    return files.has('binding.gyp') || [...files.keys()].some(path => path.endsWith('.node'));
}

async function fetchTarball(name, manifest, base, {fetchImpl, signal}) {
    const tarball = new URL(manifest.dist?.tarball, base);
    if (tarball.protocol !== 'https:' && tarball.origin !== base.origin)
        throw new Error('Package tarballs must use HTTPS or the registry origin');
    const archive = await fetchImpl(tarball, {signal});
    if (!archive.ok) throw new Error(`Package download for ${name}: HTTP ${archive.status}`);
    const bytes = await responseBytes(archive);
    return {bytes, integrity: await verifyIntegrity(bytes, manifest.dist.integrity)};
}

function registryBase(registry) {
    const base = new URL(registry, globalThis.location?.href);
    if (!base.pathname.endsWith('/')) base.pathname += '/';
    return base;
}

// The package.json inside the archive must be the requested package and need no install step.
function checkedManifest(files, name, version) {
    const pkg = JSON.parse(text.decode(files.get('package.json')));
    if (pkg.name !== name || pkg.version !== version) throw new Error('npm archive identity mismatch');
    if (needsInstallStep(pkg, files)) throw new Error(`${name} needs an unsupported install script or native module`);
    return pkg;
}

export async function downloadPackage(name, requested, {registry, fetchImpl = fetch, signal}) {
    if (!safeName(name)) throw new Error(`Unsupported npm package name: ${name}`);
    const base = registryBase(registry);
    const context = {fetchImpl, signal};
    const {version, manifest} = selectVersion(await fetchPackument(name, base, context), name, requested);
    const {bytes, integrity} = await fetchTarball(name, manifest, base, context);
    const files = await readArchive(bytes);
    const pkg = checkedManifest(files, name, version);
    return {id: idOf(name, version), key: cacheKey(name, version, integrity), manifest: pkg, integrity, files};
}

// The installed version satisfying a range, so that a later install never moves or replaces it silently.
function lockedNode(previous, name, range) {
    if (!semver.validRange(range)) return null;
    return Object.values(previous?.packages ?? {}).filter(node => node.name === name && semver.satisfies(node.version, range))
        .sort((a, b) => semver.rcompare(a.version, b.version))[0] ?? null;
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
function peerOf(node, [name, range], {nodes, roots}) {
    const peer = roots[name]?.id ?? node.dependencies[name];
    if (!peer) {
        if (node.manifest.peerDependenciesMeta?.[name]?.optional) return null;
        throw new Error(`${node.name}@${node.version} requires peer ${name}@${range}; install a compatible root explicitly`);
    }
    if (!semver.satisfies(nodes[peer].version, range))
        throw new Error(`${node.name}@${node.version} requires peer ${name}@${range}; install a compatible root explicitly`);
    return peer;
}

function connectPeers(nodes, roots) {
    for (const node of Object.values(nodes))
        for (const [name, range] of Object.entries(node.manifest.peerDependencies ?? {})) {
            const peer = peerOf(node, [name, range], {nodes, roots});
            if (peer) node.dependencies[name] = peer;
        }
}

function addNode(state, name, pkg) {
    if (Object.keys(state.nodes).length >= 512) throw new Error('Dependency graph exceeds 512 packages');
    for (const bytes of pkg.files.values()) state.total += bytes.length;
    if (state.total > 256 * 1024 * 1024) throw new Error('Dependency graph exceeds 256 MiB');
    state.nodes[pkg.id] = {name, version: pkg.manifest.version, integrity: pkg.integrity, key: pkg.key,
        manifest: pkg.manifest, files: [...pkg.files.keys()].sort(), dependencies: {}};
    state.downloads.set(pkg.id, pkg.files);
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
    const roots = {};
    for (const spec of specifications) {
        const {name, version} = parseSpec(spec);
        roots[name] = {requested: version || 'latest', id: await visit(name, version || 'latest')};
    }
    connectPeers(state.nodes, roots);
    return {lock: {format: 1, registry: options.registry ?? 'https://registry.npmjs.org/', roots, packages: state.nodes}, downloads: state.downloads};
}

function validIdentity(id, node) {
    if (!safeName(node.name)) return false;
    if (!semver.valid(node.version)) return false;
    return id === idOf(node.name, node.version);
}

function validSource(node) {
    if (!node.integrity?.startsWith('sha512-')) return false;
    return node.key === cacheKey(node.name, node.version, node.integrity);
}

function validateNode(lock, id, node) {
    if (!validIdentity(id, node) || !validSource(node)) throw new Error('Invalid locked package');
    for (const path of node.files) safePackagePath(path);
    if (Object.values(node.dependencies).some(dependency => !lock.packages[dependency]))
        throw new Error('Dangling locked dependency');
}

export function validateLock(lock) {
    if (lock?.format !== 1 || !lock.roots || !lock.packages) throw new Error('Unsupported package lock');
    for (const [id, node] of Object.entries(lock.packages)) validateNode(lock, id, node);
    if (Object.values(lock.roots).some(root => !lock.packages[root.id])) throw new Error('Dangling locked root');
    return lock;
}
