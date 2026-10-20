// npm library resolution for the browser. Tools continue to use the existing wf installer.
import semver from 'semver';
import {responseBytes} from './bootstrap.mjs';
import {gunzip, readTar, verifyIntegrity, parseSpec} from '../components/tools/wf.mjs';

export function safePackagePath(path) {
    if (typeof path !== 'string' || !path || path.includes('\\') || path.includes('\0') ||
        path.split('/').some(part => !part || part === '.' || part === '..') || /[:*?"<>|]/.test(path))
        throw new Error(`Unsafe package path: ${path}`);
    return path;
}

const text = new TextDecoder();
const safeName = name => /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/.test(name);
const idOf = (name, version) => `${name.replace('/', '+')}@${version}`;
const cacheKey = (name, version, integrity) => `${idOf(name, version)}-${integrity.slice(7).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '')}`;

export async function downloadPackage(name, requested, registry, fetchImpl = fetch, signal) {
    if (!safeName(name)) throw new Error(`Unsupported npm package name: ${name}`);
    const base = new URL(registry, globalThis.location?.href);
    if (!base.pathname.endsWith('/')) base.pathname += '/';
    const response = await fetchImpl(new URL(name.replace('/', '%2f'), base), {signal});
    if (!response.ok) throw new Error(`Registry metadata for ${name}: HTTP ${response.status}`);
    const packument = await response.json();
    const spec = requested || 'latest';
    const version = packument['dist-tags']?.[spec] ??
        (semver.valid(spec) ? spec : semver.maxSatisfying(Object.keys(packument.versions ?? {}), spec));
    const manifest = packument.versions?.[version];
    if (!manifest || !semver.valid(version)) throw new Error(`Cannot resolve ${name}@${spec}`);
    const tarball = new URL(manifest.dist?.tarball, base);
    if (tarball.protocol !== 'https:' && tarball.origin !== base.origin)
        throw new Error('Package tarballs must use HTTPS or the registry origin');
    const archive = await fetchImpl(tarball, {signal});
    if (!archive.ok) throw new Error(`Package download for ${name}: HTTP ${archive.status}`);
    const bytes = await responseBytes(archive);
    const integrity = await verifyIntegrity(bytes, manifest.dist.integrity);
    const files = new Map();
    for (const entry of readTar(await gunzip(bytes))) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid npm tarball root');
        const path = safePackagePath(entry.path.slice(8));
        if (files.has(path)) throw new Error(`Duplicate package path: ${path}`);
        files.set(path, entry.bytes);
    }
    const pkg = JSON.parse(text.decode(files.get('package.json')));
    if (pkg.name !== name || pkg.version !== version) throw new Error('npm archive identity mismatch');
    const scripts = pkg.scripts ?? {};
    if (['preinstall', 'install', 'postinstall'].some(key => scripts[key]) || pkg.gypfile || files.has('binding.gyp') ||
        [...files.keys()].some(path => path.endsWith('.node')))
        throw new Error(`${name} needs an unsupported install script or native module`);
    return {id: idOf(name, version), key: cacheKey(name, version, integrity), manifest: pkg, integrity, files};
}

// A distinct node per exact version; edges are retained instead of flattening node_modules.
export async function resolvePackages(specifications, options = {}) {
    const {registry = 'https://registry.npmjs.org/', fetchImpl = fetch, signal, download = downloadPackage} = options;
    const roots = {};
    const nodes = {};
    const downloads = new Map();
    const requests = new Map();
    let total = 0;
    const visit = async (name, range) => {
        signal?.throwIfAborted();
        const request = `${name}@${range}`;
        let pkg = requests.get(request);
        if (!pkg) {
            pkg = await download(name, range, registry, fetchImpl, signal);
            requests.set(request, pkg);
        }
        if (nodes[pkg.id]) return pkg.id; // includes cycles currently being resolved
        if (Object.keys(nodes).length >= 512) throw new Error('Dependency graph exceeds 512 packages');
        for (const bytes of pkg.files.values()) total += bytes.length;
        if (total > 256 * 1024 * 1024) throw new Error('Dependency graph exceeds 256 MiB');
        nodes[pkg.id] = {name, version: pkg.manifest.version, integrity: pkg.integrity, key: pkg.key,
            manifest: pkg.manifest, files: [...pkg.files.keys()].sort(), dependencies: {}};
        downloads.set(pkg.id, pkg.files);
        for (const [dependency, version] of Object.entries(pkg.manifest.dependencies ?? {}).sort())
            nodes[pkg.id].dependencies[dependency] = await visit(dependency, version);
        // Optional dependencies are intentionally not installed in v1. An import of one fails clearly.
        return pkg.id;
    };
    for (const spec of specifications) {
        const {name, version} = parseSpec(spec);
        roots[name] = {requested: version || 'latest', id: await visit(name, version || 'latest')};
    }
    for (const node of Object.values(nodes)) {
        for (const [name, range] of Object.entries(node.manifest.peerDependencies ?? {})) {
            const peer = roots[name]?.id ?? node.dependencies[name];
            if (!peer && node.manifest.peerDependenciesMeta?.[name]?.optional) continue;
            if (!peer || !semver.satisfies(nodes[peer].version, range))
                throw new Error(`${node.name}@${node.version} requires peer ${name}@${range}; install a compatible root explicitly`);
            node.dependencies[name] = peer;
        }
    }
    return {lock: {format: 1, registry, roots, packages: nodes}, downloads};
}

export function validateLock(lock) {
    if (lock?.format !== 1 || !lock.roots || !lock.packages) throw new Error('Unsupported package lock');
    for (const [id, node] of Object.entries(lock.packages)) {
        if (!safeName(node.name) || !semver.valid(node.version) || id !== idOf(node.name, node.version) ||
            node.key !== cacheKey(node.name, node.version, node.integrity) || !node.integrity?.startsWith('sha512-'))
            throw new Error('Invalid locked package');
        for (const path of node.files) safePackagePath(path);
        for (const dependency of Object.values(node.dependencies))
            if (!lock.packages[dependency]) throw new Error('Dangling locked dependency');
    }
    for (const root of Object.values(lock.roots))
        if (!lock.packages[root.id]) throw new Error('Dangling locked root');
    return lock;
}
