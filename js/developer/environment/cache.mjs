// The package cache: one directory per locked archive, then a record of its files' digests. The record is written
// last, so a package without one (an interrupted store) is missing, and files differing from it are corrupted.
import {call, blockSize, publish, readProject} from './io.mjs';
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
    let bytes;
    try { bytes = await readProject(bridge, recordPath(node)); }
    catch (error) {
        if (unreadable(error)) return null;
        throw error;
    }
    // Only a malformed record is a corruption; a bridge failure is reported as such.
    try { return JSON.parse(decoder.decode(bytes)); } catch { return {}; }
}

const sameFiles = (record, node) => Object.keys(record.files).sort().join('\n') === [...node.files].sort().join('\n');

// A cached file, or null when it is missing or larger than `limit`: reading stops as soon as the limit is passed,
// so an oversized file is a corruption to repair, not a read error.
async function readCached(bridge, path, limit) {
    const chunks = [];
    let size = 0;
    for (;;) {
        let chunk;
        try { chunk = await call(bridge, 'read', path, size); }
        catch (error) {
            if (unreadable(error)) return null;
            throw error;
        }
        size += chunk.length;
        if (size > limit) return null;
        chunks.push(chunk);
        if (chunk.length < blockSize) return new Uint8Array(await new Blob(chunks).arrayBuffer());
    }
}

/**
 * The cached files of a locked package, checked against its record: {state: 'valid' | 'missing' | 'corrupted', size}.
 * `size` is the unpacked size of a valid package; a package larger than `limit` bytes is corrupted.
 */
export async function inspectCache(bridge, node, {signal, limit = Infinity} = {}) {
    const record = await readRecord(bridge, node);
    if (!record) return {state: 'missing', size: 0};
    if (record.integrity !== node.integrity || !isPlainObject(record.files) || !sameFiles(record, node)) return {state: 'corrupted', size: 0};
    let size = 0;
    for (const path of node.files) {
        signal?.throwIfAborted();
        const bytes = await readCached(bridge, `${packageDirectory(node)}/${path}`, limit - size);
        if (!bytes || await sha256(bytes) !== record.files[path]) return {state: 'corrupted', size: 0};
        size += bytes.length;
    }
    return {state: 'valid', size};
}

/** 'valid', 'missing' or 'corrupted': see inspectCache(). */
export const cacheState = async (bridge, node, options) => (await inspectCache(bridge, node, options)).state;
