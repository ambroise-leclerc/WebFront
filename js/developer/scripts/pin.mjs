// Checks include/webfront/system/DeveloperReleaseData.hpp against the published distribution it pins, for sources that
// build a newer version, with wf:
//
//   wf run js/developer/scripts/pin.mjs
//
// The header must be exactly the one generated for its version, and the archive at its URL must have its integrity.
import {baseUrl, bootstrapHeader, sriOf} from '../tools/distribution.mjs';

/** The numeric core and the dot-separated prerelease identifiers of a version (the names of distribution.json). */
function versionOrder(version) {
    const match = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/.exec(version);
    if (!match) throw new Error(`Invalid version ${version}`);
    return {core: match.slice(1, 4).map(Number), prerelease: match[4]?.split('.')};
}

const numeric = identifier => /^\d+$/.test(identifier);

/** The precedence of two prerelease identifiers (SemVer 2.0.0, 11.4): numbers numerically, below text, text in ASCII order. */
function compareIdentifiers(a, b) {
    if (numeric(a) && numeric(b)) return Math.sign(Number(a) - Number(b));
    if (numeric(a)) return -1;
    if (numeric(b)) return 1;
    return a < b ? -1 : a > b ? 1 : 0;
}

/** The SemVer 2.0.0 precedence of two versions: negative when a precedes b, 0 when they are equal, positive otherwise. */
export function compareVersions(a, b) {
    const left = versionOrder(a);
    const right = versionOrder(b);
    for (let index = 0; index < 3; index++) if (left.core[index] !== right.core[index]) return Math.sign(left.core[index] - right.core[index]);
    // A prerelease precedes its release.
    if (!left.prerelease || !right.prerelease) return (left.prerelease ? -1 : 0) - (right.prerelease ? -1 : 0);
    for (let index = 0; index < Math.min(left.prerelease.length, right.prerelease.length); index++) {
        const order = compareIdentifiers(left.prerelease[index], right.prerelease[index]);
        if (order !== 0) return order;
    }
    // With equal leading identifiers, the shorter list precedes the longer one.
    return Math.sign(left.prerelease.length - right.prerelease.length);
}

/** Whether version a precedes version b. */
export function precedes(a, b) {
    return compareVersions(a, b) < 0;
}

/**
 * Where a pinned URL stands relative to the distribution's base URL (ADR-021, decision 2). Returns `{waiver, warning}`:
 * `waiver` is true when the pin is tolerated only by the declared migration waiver. The waiver is `previousBase` of
 * distribution.json with `baseSince`, the first version published under `baseUrl`; it is bounded: it covers only a pin
 * under `previousBase` whose version precedes `baseSince`. Throws when the pinned URL is off the origin of the base without
 * a valid waiver, when the waiver is incomplete, when the pin is not before `baseSince`, or when the waiver is stale
 * (the pin is on the current base, so `previousBase` and `baseSince` must be removed).
 */
export function pinOrigin(pinnedUrl, base, previousBase, pinnedVersion, baseSince) {
    const current = new URL(baseUrl(base));
    const pinned = new URL(pinnedUrl);
    const previous = previousBase === undefined ? undefined : new URL(baseUrl(previousBase));
    if (previous && previous.origin === current.origin) throw new Error('previousBase must be on another origin than baseUrl');
    if (previous && baseSince === undefined) throw new Error('previousBase requires baseSince, the first version published under baseUrl');
    if (!previous && baseSince !== undefined) throw new Error('baseSince is only meaningful with previousBase');
    if (pinned.origin === current.origin) {
        if (previous) throw new Error(`Stale migration waiver: the header pins a version published under ${current.origin}; remove previousBase and baseSince from distribution.json`);
        return {waiver: false};
    }
    if (!previous) throw new Error(`The pinned ${pinned.href} is not on ${current.origin}, the origin of baseUrl, and distribution.json declares no previousBase`);
    if (!pinned.href.startsWith(previous.href)) throw new Error(`The pinned ${pinned.href} is on neither ${current.origin} nor the declared previousBase ${previous.href}`);
    if (!precedes(pinnedVersion, baseSince)) throw new Error(`The migration waiver covers only versions before ${baseSince}, the first published under ${current.origin}; ${pinnedVersion} must be pinned on ${current.origin}`);
    return {waiver: true, warning: `migration waiver: the pinned ${pinned.href} is under the previous base ${previous.href}; it is not checked for redirection or CORS, and a browser cannot start from it if it redirects`};
}

export default async function (context) {
    const {project} = context;
    const header = await project.readText('include/webfront/system/DeveloperReleaseData.hpp');
    const field = name => header.match(new RegExp(`\\b${name}\\{"([^"]*)"\\}`))?.[1] ?? '';
    const version = field('version');
    const integrity = field('integrity');
    // The base is the one the pinned URL was published under, not distribution.json's: the sources may have moved on.
    const url = field('environment');
    const marker = `developer/${version}/environment.tgz`;
    if (!url.endsWith('/' + marker) || header !== bootstrapHeader(version, url.slice(0, -marker.length), integrity))
        throw new Error(`DeveloperReleaseData.hpp is not the header generated for the published ${version}`);
    // (ii) The pinned URL itself: on the origin of baseUrl, or under the declared migration waiver (ADR-021).
    const distribution = JSON.parse(await project.readText('js/developer/tools/distribution.json'));
    const current = baseUrl(distribution.baseUrl);
    const {waiver, warning} = pinOrigin(url, current, distribution.previousBase, version, distribution.baseSince);
    // (i) The bytes, fetched where the distribution is served now: the pinned integrity proves the file.
    const served = new URL(marker, current).href;
    const response = await context.fetch(served, {signal: context.signal});
    if (!response.ok) throw new Error(`The pinned environment ${served} is not published (${response.status})`);
    if (await sriOf(new Uint8Array(await response.arrayBuffer())) !== integrity)
        throw new Error(`The published ${served} does not have the pinned integrity`);
    if (waiver) {
        context.console.warn(`WARNING: ${warning}`);
    } else if (served !== url) {
        // (ii) and (iii): the script's fetch rejects a redirect, and a cross-origin fetch from its Worker succeeds only
        // with valid CORS headers.
        let pinned;
        try {
            pinned = await context.fetch(url, {signal: context.signal});
        } catch (error) {
            throw new Error(`The pinned ${url} cannot be fetched without redirection and with CORS headers: ${error.message}`);
        }
        if (pinned.status !== 200) throw new Error(`The pinned ${url} answers ${pinned.status}, not 200`);
        if (await sriOf(new Uint8Array(await pinned.arrayBuffer())) !== integrity)
            throw new Error(`The archive at the pinned ${url} does not have the pinned integrity`);
    }
    context.console.log(`DeveloperReleaseData.hpp pins the published ${version}.`);
}
