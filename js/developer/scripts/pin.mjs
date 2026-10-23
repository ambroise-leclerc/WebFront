// Checks include/webfront/system/DeveloperReleaseData.hpp against the published distribution it pins, for sources that
// build a newer version, with wf:
//
//   wf run js/developer/scripts/pin.mjs
//
// The header must be exactly the one generated for its version, and the archive at its URL must have its integrity.
import {bootstrapHeader, sriOf} from '../tools/distribution.mjs';

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
    const response = await context.fetch(url, {signal: context.signal});
    if (!response.ok) throw new Error(`The pinned environment ${url} is not published (${response.status})`);
    if (await sriOf(new Uint8Array(await response.arrayBuffer())) !== integrity)
        throw new Error(`The published ${url} does not have the pinned integrity`);
    context.console.log(`DeveloperReleaseData.hpp pins the published ${version}.`);
}
