export const packageRoot = '.webfront/developer/packages';

const forbidden = /[\\\0:*?"<>|]/;
const hasUnsafePart = path => path.split('/').some(part => !part || part === '.' || part === '..');

export function isUnsafe(path) {
    if (typeof path !== 'string' || !path) return true;
    if (forbidden.test(path)) return true;
    return hasUnsafePart(path);
}

export function safePackagePath(path) {
    if (isUnsafe(path)) throw new Error(`Unsafe package path: ${path}`);
    return path;
}
