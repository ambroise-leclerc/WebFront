// The project as the wf commands see it: manifest, lock, whether they agree, and their joint publication.
import {call, readProject, writeChunks} from './io.mjs';
import {manifestPath, parseManifest, dependenciesDigest} from './manifest.mjs';
import {validateLock} from './lock.mjs';

export const lockPath = 'webfront-packages.lock.json';
export const packageRoot = '.webfront/developer/packages';

const decoder = new TextDecoder();
const encoder = new TextEncoder();

export const noManifest = 'No webfront.project.json: run wf init';
export const desynchronized = 'The lock does not match the dependencies of webfront.project.json; run wf install --update to resolve them again';

async function readOptional(bridge, path) {
    try { return await readProject(bridge, path); }
    catch (error) {
        if (error.message.includes('Cannot read project file:')) return null;
        throw error;
    }
}

function statusOf(manifest, lock, digest) {
    if (!manifest) return 'no-manifest';
    if (!lock) return 'unlocked';
    return lock.manifestDigest === digest ? 'consistent' : 'desynchronized';
}

/**
 * Reads the manifest and the lock and compares them. `token` identifies the exact state that was read:
 * a publication prepared from it is refused if either file changed in the meantime.
 *   no-manifest: no webfront.project.json; unlocked: no lock, so no installed graph;
 *   consistent: the lock was resolved from the manifest's current dependencies;
 *   desynchronized: the dependencies changed since; nothing is resolved implicitly.
 */
export async function loadProject(bridge) {
    const token = await call(bridge, 'state', `${manifestPath}\n${lockPath}`);
    const manifestBytes = await readOptional(bridge, manifestPath);
    const lockBytes = await readOptional(bridge, lockPath);
    const manifest = manifestBytes ? parseManifest(manifestBytes) : null;
    const lock = lockBytes ? validateLock(JSON.parse(decoder.decode(lockBytes))) : null;
    const digest = manifest ? await dependenciesDigest(manifest.dependencies) : null;
    return {manifest, lock, token, status: statusOf(manifest, lock, digest), digest};
}

/** A line to put before the output of a command that reads a lock which no longer matches its manifest. */
export const syncNotice = project => project.status === 'desynchronized' ? `Warning: ${desynchronized}.\r\n` : '';

/**
 * Publishes the given files (path -> bytes) as one change, on condition that the project is still in the
 * state `project` was read in. A refusal means another installation finished first: run the command again.
 */
export async function publishTogether(bridge, files, project, {signal} = {}) {
    const ids = [];
    try {
        for (const [path, bytes] of files) {
            signal?.throwIfAborted();
            const id = await call(bridge, 'begin', path, false);
            ids.push(id);
            await writeChunks(bridge, {id, path: 'output', bytes}, signal);
        }
        signal?.throwIfAborted();
        await call(bridge, 'commitGroup', ids.join('\n'), `${manifestPath}\n${lockPath}`, project.token);
    } catch (error) {
        for (const id of ids) await call(bridge, 'abort', id).catch(() => {});
        throw error;
    }
}

/** An entry given by its name in the manifest's "entries", or as a project path. */
export function resolveEntry(manifest, argument, command) {
    const entries = manifest?.entries ?? {};
    if (argument !== undefined) return Object.hasOwn(entries, argument) ? entries[argument] : argument;
    if (Object.hasOwn(entries, 'main')) return entries.main;
    const names = Object.keys(entries);
    if (names.length === 1) return entries[names[0]];
    throw new Error(`Usage: ${command} (declare entries.main in ${manifestPath} to omit the entry)`);
}

export const textBytes = text => encoder.encode(text);
