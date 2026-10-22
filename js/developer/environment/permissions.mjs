// The permissions of webfront.project.json, applied to the operations of the script `context` API. They frame what
// a script asks WebFront to do; they do not restrict the Worker's own capabilities (global fetch, importScripts,
// WebSocket...) and are not a sandbox.
import {safePackagePath} from './paths.mjs';
import {manifestPath} from './manifest.mjs';
import {lockPath} from './project.mjs';

// wf keeps these consistent itself: a script never writes them, whatever the manifest grants.
const reserved = [manifestPath, lockPath, '.webfront'];

const within = (path, root) => path === root || path.startsWith(root + '/');

function projectPath(path) {
    try { return safePackagePath(path); }
    catch { throw new Error(`Not a project-relative path: ${path}`); }
}

const denied = (operation, path, key) =>
    new Error(`Permission denied: ${operation} ${path} is not granted by permissions.${key} in ${manifestPath}`);

/** A path the context may read or list: under permissions.files, or under a path it may write. */
export function checkRead(permissions, path) {
    const checked = projectPath(path);
    const granted = [...permissions?.files ?? [], ...permissions?.publish ?? []];
    if (!granted.some(root => within(checked, root))) throw denied('reading', path, 'files');
    return checked;
}

/** A path the context may publish: under permissions.publish, never the manifest, the lock or .webfront/. */
export function checkWrite(permissions, path) {
    const checked = projectPath(path);
    if (reserved.some(root => within(checked, root))) throw new Error(`Permission denied: ${path} is managed by wf and never written by a script`);
    if (!(permissions?.publish ?? []).some(root => within(checked, root))) throw denied('writing', path, 'publish');
    return checked;
}

/** A URL context.fetch may request: its origin is listed in permissions.fetch. */
export function checkFetch(permissions, url, base) {
    const target = new URL(url, base);
    if (!(permissions?.fetch ?? []).includes(target.origin)) throw denied('fetching', target.origin, 'fetch');
    return target;
}
