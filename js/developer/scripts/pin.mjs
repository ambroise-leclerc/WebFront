// Checks include/webfront/system/DeveloperReleaseData.hpp against the published distribution it pins, for sources that
// build a newer version, with wf:
//
//   wf run js/developer/scripts/pin.mjs
//
// The header must be exactly the one generated for its version, and the archive at its URL must have its integrity.
import {baseUrl, bootstrapHeader, sriOf} from '../tools/distribution.mjs';

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
