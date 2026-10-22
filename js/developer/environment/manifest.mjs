// webfront.project.json, the project manifest every wf command reads: intent (roots and ranges), entries,
// providers and permissions. The lock records the exact resolution.
import {safePackagePath} from './paths.mjs';
import {isPlainObject} from './entry.mjs';

export const manifestPath = 'webfront.project.json';
export const lockPath = 'webfront-packages.lock.json';
export const manifestFormat = 1;

const npmName = /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/;
const exactVersion = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/;
// The shape of an npm range or dist-tag; the registry resolution applies the semver semantics.
const rangeShape = /^[0-9A-Za-z.+\-^~<>=*| ]{1,128}$/;
const label = /^[A-Za-z0-9][A-Za-z0-9:_.-]{0,63}$/;
const identifier = /^[A-Za-z_][A-Za-z0-9_]*$/;
const keys = ['format', 'name', 'version', 'dependencies', 'registry', 'scripts', 'entries', 'providers', 'test', 'permissions'];
const permissionKeys = ['files', 'publish', 'fetch'];

const fail = message => { throw new Error(`${manifestPath}: ${message}`); };

function requireObject(value, where) {
    if (!isPlainObject(value)) fail(`${where} must be an object`);
    return value;
}

function onlyKeys(object, allowed, where) {
    for (const key of Object.keys(object))
        if (!allowed.includes(key)) fail(`unknown key "${key}" in ${where}`);
}

function projectPath(value, where) {
    if (typeof value !== 'string') fail(`${where} must be a project path`);
    try { return safePackagePath(value); } catch { return fail(`${where} is not a safe project-relative path: ${value}`); }
}

function pathList(value, where) {
    if (!Array.isArray(value) || value.length > 256) fail(`${where} must be an array of at most 256 paths`);
    return value.map((path, index) => projectPath(path, `${where}[${index}]`));
}

function checkDependencies(value) {
    const dependencies = requireObject(value, 'dependencies');
    for (const [name, range] of Object.entries(dependencies)) {
        if (!npmName.test(name)) fail(`invalid npm package name "${name}"`);
        if (typeof range !== 'string' || !rangeShape.test(range) || range !== range.trim())
            fail(`invalid version range for "${name}"`);
    }
    return dependencies;
}

// A path that stays on the application's origin: browsers read "//host" and "/\\host" as another host.
export const sameOriginPath = value => value.startsWith('/') && !value.startsWith('//') && !value.includes('\\');

function checkRegistry(value) {
    if (typeof value !== 'string') fail('registry must be a URL');
    if (sameOriginPath(value)) return value;
    let url;
    try { url = new URL(value); } catch { return fail('registry must be an https URL or an absolute path'); }
    if (url.protocol !== 'https:') fail('registry must use https');
    return value;
}

function checkScripts(value) {
    const scripts = requireObject(value, 'scripts');
    for (const [name, command] of Object.entries(scripts))
        if (!label.test(name) || typeof command !== 'string' || !command || command.length > 1024) fail(`invalid script "${name}"`);
    return scripts;
}

function checkEntries(value) {
    const entries = requireObject(value, 'entries');
    for (const [name, path] of Object.entries(entries)) {
        if (!label.test(name)) fail(`invalid entry name "${name}"`);
        projectPath(path, `entries.${name}`);
    }
    return entries;
}

function checkProviders(value, entries) {
    const providers = requireObject(value, 'providers');
    for (const [name, provider] of Object.entries(providers)) {
        if (!identifier.test(name)) fail(`invalid provider name "${name}"`);
        requireObject(provider, `providers.${name}`);
        onlyKeys(provider, ['entry', 'output'], `providers.${name}`);
        if (typeof provider.entry !== 'string') fail(`providers.${name}.entry must name an entry or a path`);
        if (!Object.hasOwn(entries, provider.entry)) projectPath(provider.entry, `providers.${name}.entry`);
        if (!projectPath(provider.output, `providers.${name}.output`).endsWith('.hpp')) fail(`providers.${name}.output must be a .hpp file`);
    }
    return providers;
}

function checkTest(value) {
    const test = requireObject(value, 'test');
    onlyKeys(test, ['specs'], 'test');
    if (test.specs !== undefined) pathList(test.specs, 'test.specs');
    return test;
}

function checkOrigin(origin, where) {
    let url;
    try { url = new URL(origin); } catch { return fail(`${where} must be an https origin`); }
    if (url.protocol !== 'https:' || url.origin !== origin) fail(`${where} must be an https origin without a path`);
    return origin;
}

function checkPermissions(value) {
    const permissions = requireObject(value, 'permissions');
    for (const key of Object.keys(permissions))
        if (!permissionKeys.includes(key)) fail(`unknown permission "${key}"`);
    for (const key of ['files', 'publish'])
        if (permissions[key] !== undefined) pathList(permissions[key], `permissions.${key}`);
    if (permissions.fetch !== undefined) {
        if (!Array.isArray(permissions.fetch) || permissions.fetch.length > 64) fail('permissions.fetch must be an array of origins');
        permissions.fetch.forEach((origin, index) => checkOrigin(origin, `permissions.fetch[${index}]`));
    }
    return permissions;
}

function checkIdentity(manifest) {
    if (typeof manifest.name !== 'string' || !npmName.test(manifest.name)) fail('name must be a lower-case npm package name');
    if (typeof manifest.version !== 'string' || !exactVersion.test(manifest.version)) fail('version must be a semantic version');
}

function checkFormat(manifest) {
    if (manifest.format === manifestFormat) return;
    if (Number.isInteger(manifest.format) && manifest.format > manifestFormat)
        fail(`format ${manifest.format} is not supported by this environment (it reads format ${manifestFormat}); update the environment`);
    fail(`format must be ${manifestFormat}`);
}

/** Validates a parsed manifest; every problem is an explicit error, never a silent fallback. */
export function validateManifest(manifest) {
    requireObject(manifest, 'the manifest');
    checkFormat(manifest);
    onlyKeys(manifest, keys, 'the manifest');
    checkIdentity(manifest);
    const entries = manifest.entries === undefined ? {} : checkEntries(manifest.entries);
    return {
        ...manifest,
        dependencies: manifest.dependencies === undefined ? {} : checkDependencies(manifest.dependencies),
        entries,
        providers: manifest.providers === undefined ? {} : checkProviders(manifest.providers, entries),
        ...(manifest.registry === undefined ? {} : {registry: checkRegistry(manifest.registry)}),
        ...(manifest.scripts === undefined ? {} : {scripts: checkScripts(manifest.scripts)}),
        ...(manifest.test === undefined ? {} : {test: checkTest(manifest.test)}),
        ...(manifest.permissions === undefined ? {} : {permissions: checkPermissions(manifest.permissions)}),
    };
}

/** The manifest as written (`source`, to update without adding the defaults) and validated (`manifest`). */
export function readManifest(bytes) {
    let source;
    try { source = JSON.parse(new TextDecoder().decode(bytes)); } catch { return fail('not valid JSON'); }
    return {source, manifest: validateManifest(source)};
}

export function parseManifest(bytes) {
    return readManifest(bytes).manifest;
}

/** The text written to disk: the schema's keys in a stable order. */
export function serializeManifest(manifest) {
    const ordered = {};
    for (const key of keys) if (manifest[key] !== undefined) ordered[key] = manifest[key];
    return JSON.stringify(ordered, null, 2) + '\n';
}

export function newManifest(name) {
    return {format: manifestFormat, name, version: '0.1.0', dependencies: {}};
}

/** A package name derived from a directory name, for wf init. */
export function manifestName(directory) {
    const base = String(directory ?? '').split('/').filter(Boolean).pop() ?? '';
    const name = base.toLowerCase().replace(/[^a-z0-9._-]+/g, '-').replace(/^[^a-z0-9]+/, '').replace(/-+$/, '');
    return npmName.test(name) ? name : 'webfront-app';
}

/** Fingerprint of the manifest's dependency intent, recorded by the lock it produced. */
export async function dependenciesDigest(dependencies) {
    const canonical = JSON.stringify(Object.entries(dependencies).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0));
    const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(canonical));
    return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

export {exactVersion, npmName, identifier};
