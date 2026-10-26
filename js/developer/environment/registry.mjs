// npm registry access: resolving a version, downloading and checking one package archive.
import semver from 'semver';
import {responseBytes} from './io.mjs';
import {verifyIntegrity} from './tarball.mjs';
import {readArchive, checkedManifest} from './archive.mjs';
import {sameOriginPath} from './manifest.mjs';

export const safeName = name => /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/.test(name);
export const idOf = (name, version) => `${name.replace('/', '+')}@${version}`;
export const cacheKey = (name, version, integrity) => `${idOf(name, version)}-${integrity.slice(7).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '')}`;

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

async function fetchTarball(name, manifest, base, {fetchImpl, signal}) {
    const tarball = new URL(manifest.dist?.tarball, base);
    if (tarball.protocol !== 'https:' && tarball.origin !== base.origin)
        throw new Error('Package tarballs must use HTTPS or the registry origin');
    const archive = await fetchImpl(tarball, {signal});
    if (!archive.ok) throw new Error(`Package download for ${name}: HTTP ${archive.status}`);
    const bytes = await responseBytes(archive);
    return {bytes, integrity: await verifyIntegrity(bytes, manifest.dist.integrity)};
}

// Like the environment archive: HTTPS, or the application's own origin for a local mirror.
// A path must also resolve on that origin, whatever the parser makes of it.
export function registryBase(registry) {
    const base = new URL(registry, globalThis.location?.href);
    const local = base.origin === globalThis.location?.origin;
    if (registry.startsWith('/') ? !sameOriginPath(registry) || !local : base.protocol !== 'https:' && !local)
        throw new Error(`The npm registry must use HTTPS or the application origin: ${registry}`);
    if (!base.pathname.endsWith('/')) base.pathname += '/';
    return base;
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
