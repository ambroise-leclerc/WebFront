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
    // The archive is fetched where the distribution is served now. A version pinned under a previous address (the
    // github.io one, which redirects to the custom domain without CORS headers, so that a browser cannot follow it) is the
    // same file there: the pinned integrity is what proves it.
    const current = baseUrl(JSON.parse(await project.readText('js/developer/tools/distribution.json')).baseUrl);
    const served = new URL(marker, current).href;
    const response = await context.fetch(served, {signal: context.signal});
    if (!response.ok) throw new Error(`The pinned environment ${served} is not published (${response.status})`);
    if (await sriOf(new Uint8Array(await response.arrayBuffer())) !== integrity)
        throw new Error(`The published ${served} does not have the pinned integrity`);
    context.console.log(`DeveloperReleaseData.hpp pins the published ${version}.`);
    // The check above proves the bytes, not that a browser can start from the pinned address: when that address is not
    // the one the distribution is served from, the pinned URL redirects without CORS headers and an application built with
    // this header cannot start its developer environment until a version pinned on the current address is published.
    if (new URL(url).origin !== new URL(current).origin)
        context.console.log(`WARNING: the pinned ${url} is not on ${new URL(current).origin}; its redirect carries no CORS headers, so a browser cannot start the developer environment from this pin.`);
}
