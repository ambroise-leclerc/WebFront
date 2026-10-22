// The package cache: one directory per locked archive, then a record of its files' digests. The record is written
// last, so a package without one (an interrupted store) is missing, and files differing from it are corrupted.
import {publish, readProject} from './io.mjs';
import {isPlainObject} from './entry.mjs';
import {packageRoot} from './project.mjs';

export const packageDirectory = node => `${packageRoot}/${node.key}`;
export const recordPath = node => `${packageRoot}/${node.key}.json`;

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const unreadable = error => error.message.includes('Cannot read project file:');

async function sha256(bytes) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
    return [...digest].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

/** Publishes a verified archive's files, then the record that makes them a cached package. */
export async function storePackage(bridge, node, files, {signal} = {}) {
    const digests = {};
    for (const path of [...files.keys()].sort()) digests[path] = await sha256(files.get(path));
    await publish(bridge, packageDirectory(node), files, {signal});
    const record = JSON.stringify({format: 1, integrity: node.integrity, files: digests}, null, 2) + '\n';
    await publish(bridge, recordPath(node), new Map([['output', encoder.encode(record)]]), {directory: false, signal});
}

async function readRecord(bridge, node) {
    try { return JSON.parse(decoder.decode(await readProject(bridge, recordPath(node)))); }
    catch (error) { return unreadable(error) ? null : {}; }
}

const sameFiles = (record, node) => Object.keys(record.files).sort().join('\n') === [...node.files].sort().join('\n');

async function fileMatches(bridge, node, path, digest) {
    try { return await sha256(await readProject(bridge, `${packageDirectory(node)}/${path}`)) === digest; }
    catch (error) {
        if (unreadable(error)) return false;
        throw error;
    }
}

/** 'valid', 'missing' or 'corrupted': the cached files of a locked package, checked against its record. */
export async function cacheState(bridge, node, {signal} = {}) {
    const record = await readRecord(bridge, node);
    if (!record) return 'missing';
    if (record.integrity !== node.integrity || !isPlainObject(record.files) || !sameFiles(record, node)) return 'corrupted';
    for (const path of node.files) {
        signal?.throwIfAborted();
        if (!await fileMatches(bridge, node, path, record.files[path])) return 'corrupted';
    }
    return 'valid';
}
