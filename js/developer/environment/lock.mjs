// Validation of webfront-packages.lock.json before any of its files is read.
import semver from 'semver';
import {safeName, idOf, cacheKey} from './registry.mjs';
import {safePackagePath} from './paths.mjs';

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

function hasLockShape(lock) {
    if (lock?.format !== 1) return false;
    return Boolean(lock.roots) && Boolean(lock.packages);
}

export function validateLock(lock) {
    if (!hasLockShape(lock)) throw new Error('Unsupported package lock');
    for (const [id, node] of Object.entries(lock.packages)) validateNode(lock, id, node);
    if (Object.values(lock.roots).some(root => !lock.packages[root.id])) throw new Error('Dangling locked root');
    return lock;
}
