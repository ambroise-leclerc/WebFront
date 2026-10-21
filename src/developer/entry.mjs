// Package entry points: normalized paths and the "exports" map of package.json.
import {conditional, isPlainObject} from './conditions.mjs';
export {isPlainObject};

function applySegment(parts, part) {
    if (!part || part === '.') return;
    if (part !== '..') {
        parts.push(part);
        return;
    }
    if (!parts.length) throw new Error('Import escapes its source root');
    parts.pop();
}

export function normalize(path) {
    const parts = [];
    for (const part of path.split('/')) applySegment(parts, part);
    return parts.join('/');
}

function exportedPath(result, key) {
    if (!result?.startsWith('./')) throw new Error(`Unsupported package export: ${key}`);
    return normalize(result);
}

function isSubpathMap(value) {
    if (!isPlainObject(value)) return false;
    return Object.keys(value).some(key => key.startsWith('.'));
}

const matches = (pattern, key) => {
    const [before, after] = pattern.split('*');
    return key.startsWith(before) && key.endsWith(after);
};

// The longest "./prefix/*"-style subpath pattern that matches the requested subpath.
const matchingPattern = (exports, key) => Object.keys(exports).filter(candidate => candidate.includes('*'))
    .sort((a, b) => b.length - a.length).find(candidate => matches(candidate, key));

function patternEntry(exports, key, conditions) {
    const pattern = matchingPattern(exports, key);
    if (!pattern) throw new Error(`Package does not export ${key}`);
    const [before, after] = pattern.split('*');
    return exportedPath(conditional(exports[pattern], key.slice(before.length, key.length - after.length), conditions), key);
}

function legacyMain(manifest) {
    if (typeof manifest.browser === 'string' && manifest.browser) return manifest.browser;
    return manifest.module || manifest.main || 'index.js';
}

function mapEntry(exports, key, conditions) {
    if (!Object.hasOwn(exports, key)) return patternEntry(exports, key, conditions);
    return exportedPath(conditional(exports[key], undefined, conditions), key);
}

export function packageEntry(manifest, subpath = '', kind = 'import') {
    const conditions = new Set(['browser', kind, 'default', 'production']);
    const exports = manifest.exports;
    if (exports === undefined) return normalize(subpath || legacyMain(manifest));
    const key = subpath ? `./${subpath}` : '.';
    if (isSubpathMap(exports)) return mapEntry(exports, key, conditions);
    if (subpath) throw new Error(`Package does not export ${key}`);
    return exportedPath(conditional(exports, undefined, conditions), key);
}
