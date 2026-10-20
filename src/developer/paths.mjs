export function safePackagePath(path) {
    if (typeof path !== 'string' || !path || path.includes('\\') || path.includes('\0') ||
        path.split('/').some(part => !part || part === '.' || part === '..') || /[:*?"<>|]/.test(path))
        throw new Error(`Unsafe package path: ${path}`);
    return path;
}
