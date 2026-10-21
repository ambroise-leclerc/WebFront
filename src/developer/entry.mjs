// Package entry points: normalized paths and the "exports" conditions of package.json.

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

export function isPlainObject(value) {
    if (!value || typeof value !== 'object') return false;
    return !Array.isArray(value);
}

function firstMatch(candidates, wildcard, conditions) {
    for (const candidate of candidates) {
        const result = conditional(candidate, wildcard, conditions);
        if (result) return result;
    }
    return null;
}

function byCondition(object, wildcard, conditions) {
    for (const [key, target] of Object.entries(object)) {
        if (!conditions.has(key)) continue;
        const result = conditional(target, wildcard, conditions);
        if (result) return result;
    }
    return null;
}

function conditional(value, wildcard, conditions) {
    if (typeof value === 'string') return value.replaceAll('*', wildcard ?? '');
    if (Array.isArray(value)) return firstMatch(value, wildcard, conditions);
    if (isPlainObject(value)) return byCondition(value, wildcard, conditions);
    return null;
}

function exportedPath(result, key) {
    if (!result?.startsWith('./')) throw new Error(`Unsupported package export: ${key}`);
    return normalize(result);
}

function isSubpathMap(value) {
    if (!isPlainObject(value)) return false;
    return Object.keys(value).some(key => key.startsWith('.'));
}

function patternEntry(exports, key, conditions) {
    const pattern = Object.keys(exports).filter(candidate => candidate.includes('*'))
        .sort((a, b) => b.length - a.length).find(candidate => {
            const [before, after] = candidate.split('*');
            return key.startsWith(before) && key.endsWith(after);
        });
    if (!pattern) throw new Error(`Package does not export ${key}`);
    const [before, after] = pattern.split('*');
    return exportedPath(conditional(exports[pattern], key.slice(before.length, key.length - after.length), conditions), key);
}

function legacyMain(manifest) {
    if (typeof manifest.browser === 'string' && manifest.browser) return manifest.browser;
    return manifest.module || manifest.main || 'index.js';
}

export function packageEntry(manifest, subpath = '', kind = 'import') {
    const conditions = new Set(['browser', kind, 'default', 'production']);
    const exports = manifest.exports;
    if (exports === undefined) return normalize(subpath || legacyMain(manifest));
    const key = subpath ? `./${subpath}` : '.';
    if (!isSubpathMap(exports)) {
        if (subpath) throw new Error(`Package does not export ${key}`);
        return exportedPath(conditional(exports, undefined, conditions), key);
    }
    if (Object.hasOwn(exports, key)) return exportedPath(conditional(exports[key], undefined, conditions), key);
    return patternEntry(exports, key, conditions);
}
