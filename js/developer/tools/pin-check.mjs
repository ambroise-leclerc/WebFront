// Checks DeveloperReleaseData.hpp against the published distribution it pins, for sources that build a newer version:
// the header must be exactly the one generated for its version, and the archive at its URL must have its integrity.
import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {bootstrapHeader} from './site.mjs';

const root = new URL('../../../', import.meta.url);
const header = await readFile(new URL('include/webfront/system/DeveloperReleaseData.hpp', root), 'utf8');
const distribution = JSON.parse(await readFile(new URL('js/developer/tools/distribution.json', root)));
const field = name => header.match(new RegExp(`\\b${name}\\{"([^"]*)"\\}`))?.[1] ?? '';
const version = field('version');
const integrity = field('integrity');
if (header !== bootstrapHeader(version, distribution.baseUrl, integrity))
    throw new Error(`DeveloperReleaseData.hpp is not the header generated for the published ${version}`);
const url = field('environment');
const response = await fetch(url);
if (!response.ok) throw new Error(`The pinned environment ${url} is not published (${response.status})`);
const actual = 'sha512-' + createHash('sha512').update(new Uint8Array(await response.arrayBuffer())).digest('base64');
if (actual !== integrity) throw new Error(`The published ${url} does not have the pinned integrity`);
console.log(`DeveloperReleaseData.hpp pins the published ${version}.`);
