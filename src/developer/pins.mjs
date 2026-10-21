// Keeping the versions of an installed lock: a later install never moves or replaces them silently.
import semver from 'semver';
import {downloadPackage} from './registry.mjs';

// The installed version satisfying a range, so that a later install never moves or replaces it silently.
function lockedNode(previous, name, range) {
    if (!semver.validRange(range)) return null;
    const candidates = Object.values(previous?.packages ?? {}).filter(node => node.name === name && semver.satisfies(node.version, range));
    return candidates.sort((a, b) => semver.rcompare(a.version, b.version))[0] ?? null;
}

// Downloads each name@range once; versions already in options.previous are kept and their archives must
// still match the recorded integrity.
export function pinnedDownloader({registry = 'https://registry.npmjs.org/', fetchImpl = fetch, signal, download = downloadPackage, previous}) {
    const requests = new Map();
    return async (name, range) => {
        const request = `${name}@${range}`;
        if (requests.has(request)) return requests.get(request);
        const pinned = lockedNode(previous, name, range);
        const pkg = await download(name, pinned?.version ?? range, {registry, fetchImpl, signal});
        if (pinned && pkg.integrity !== pinned.integrity)
            throw new Error(`${pkg.id} no longer matches the integrity recorded in the lock`);
        requests.set(request, pkg);
        return pkg;
    };
}
