// Conditional targets of package.json "exports": the first condition that resolves wins, like Node.

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

export function conditional(value, wildcard, conditions) {
    if (typeof value === 'string') return value.replaceAll('*', wildcard ?? '');
    if (Array.isArray(value)) return firstMatch(value, wildcard, conditions);
    if (isPlainObject(value)) return byCondition(value, wildcard, conditions);
    return null;
}
