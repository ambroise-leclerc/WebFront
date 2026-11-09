// Checks include/webfront/system/DeveloperReleaseData.hpp against the published distribution it pins, for sources that
// build a newer version, with wf:
//
//   wf run js/developer/scripts/pin.mjs
//
// The header must be exactly the one generated for its version, and the archive at its URL must have its integrity.
import {baseUrl, bootstrapHeader, sriOf} from '../tools/distribution.mjs';

/**
 * Where a pinned URL stands relative to the distribution's base URL (ADR-021, decision 2). Returns `{waiver, warning}`:
 * `waiver` is true when the pin is tolerated only by the declared migration waiver (`previousBase` of distribution.json).
 * Throws when the pinned URL is off the origin of the base without a valid waiver, or when a waiver is stale (the pin
 * is on the current base, so `previousBase` must be removed).
 */
export function pinOrigin(pinnedUrl, base, previousBase) {
    const current = new URL(baseUrl(base));
    const pinned = new URL(pinnedUrl);
    const previous = previousBase === undefined ? undefined : new URL(baseUrl(previousBase));
    if (previous && previous.origin === current.origin) throw new Error('previousBase must be on another origin than baseUrl');
    if (pinned.origin === current.origin) {
        if (previous) throw new Error(`Stale migration waiver: the header pins a version published under ${current.origin}; remove previousBase from distribution.json`);
        return {waiver: false};
    }
    if (!previous) throw new Error(`The pinned ${pinned.href} is not on ${current.origin}, the origin of baseUrl, and distribution.json declares no previousBase`);
    if (!pinned.href.startsWith(previous.href)) throw new Error(`The pinned ${pinned.href} is on neither ${current.origin} nor the declared previousBase ${previous.href}`);
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
    const {waiver, warning} = pinOrigin(url, current, distribution.previousBase);
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
