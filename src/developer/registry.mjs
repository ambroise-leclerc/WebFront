// npm registry access: resolving a version, downloading and checking one package archive.
import semver from 'semver';
import {responseBytes} from './io.mjs';
import {gunzip, readTar, verifyIntegrity} from '../components/tools/wf.mjs';
import {safePackagePath} from './paths.mjs';

const text = new TextDecoder();
export const safeName = name => /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/.test(name);
export const idOf = (name, version) => `${name.replace('/', '+')}@${version}`;
export const cacheKey = (name, version, integrity) => `${idOf(name, version)}-${integrity.slice(7).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '')}`;
const installScripts = ['preinstall', 'install', 'postinstall'];

async function fetchPackument(name, base, {fetchImpl, signal}) {
    const response = await fetchImpl(new URL(name.replace('/', '%2f'), base), {signal});
    if (!response.ok) throw new Error(`Registry metadata for ${name}: HTTP ${response.status}`);
    return response.json();
}

function resolveVersion(packument, spec) {
    const tagged = packument['dist-tags']?.[spec];
    if (tagged) return tagged;
    if (semver.valid(spec)) return spec;
    return semver.maxSatisfying(Object.keys(packument.versions ?? {}), spec);
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

const hasInstallScript = pkg => installScripts.some(key => pkg.scripts?.[key]);
const hasNativeFile = files => files.has('binding.gyp') || [...files.keys()].some(path => path.endsWith('.node'));

function needsInstallStep(pkg, files) {
    if (hasInstallScript(pkg)) return true;
    if (pkg.gypfile) return true;
    return hasNativeFile(files);
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
