// Static, version-preserving GitHub Pages distribution. No dependency on npm at consumption time.
import {readFile, writeFile, mkdir, readdir, stat} from 'node:fs/promises';
import {resolve, join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';

export function baseUrl(value) {
    const url = new URL(value);
    if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash)
        throw new Error('Distribution base URL must be an HTTPS directory URL');
    if (!url.pathname.endsWith('/')) url.pathname += '/';
    return url.href;
}
export function versionPath(version) {
    if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) throw new Error('Invalid distribution version');
    return `developer/${version}/`;
}
export function bootstrapHeader(version, base, integrity) {
    if (!/^sha512-[A-Za-z0-9+/]{86}==$/.test(integrity)) throw new Error('Invalid distribution SHA-512');
    const url = new URL(versionPath(version) + 'environment.tgz', baseUrl(base));
    return `#pragma once

// Pinned developer distribution. Regenerate with tools/developer/build.mjs --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{${JSON.stringify(version)}};
inline constexpr std::string_view environment{${JSON.stringify(url.href)}};
inline constexpr std::string_view integrity{${JSON.stringify(integrity)}};
} // namespace webfront::developer::release
`;
}

const digest = bytes => 'sha512-' + createHash('sha512').update(bytes).digest('base64');
export async function stageSite(distribution, site) {
    const manifest = JSON.parse(await readFile(join(distribution, 'provision.json')));
    const relative = versionPath(manifest.version);
    const names = ['environment.tgz', 'pi-starter.tgz', 'provision.json', 'DeveloperReleaseData.hpp'];
    const input = new Map();
    for (const name of names) input.set(name, await readFile(join(distribution, name)));
    if (digest(input.get('environment.tgz')) !== manifest.integrity ||
        digest(input.get('pi-starter.tgz')) !== manifest.starters?.pi?.integrity)
        throw new Error('Distribution archive checksum mismatch');
    const destination = join(site, relative);
    let exists = false;
    try { exists = (await stat(destination)).isDirectory(); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (exists) {
        // Check every byte before modifying anything: reruns may never replace an existing version.
        for (const [name, bytes] of input)
            if (!bytes.equals(await readFile(join(destination, name))))
                throw new Error(`Published version ${manifest.version} differs; increment the distribution version`);
    } else {
        await mkdir(destination, {recursive: true});
        for (const [name, bytes] of input) await writeFile(join(destination, name), bytes);
    }
    const versions = (await readdir(join(site, 'developer'))).filter(name => /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(name)).sort();
    await writeFile(join(site, '.nojekyll'), '');
    await writeFile(join(site, 'index.html'), `<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>WebFront developer packages</title><h1>WebFront developer packages</h1><p>Use a version-pinned bootstrap header and SHA-512. React and its dependencies are downloaded separately from npm.</p><ul>${versions.map(version => `<li>${version}: <a href="developer/${version}/provision.json">manifest</a> · <a href="developer/${version}/environment.tgz">environment</a> · <a href="developer/${version}/pi-starter.tgz">PI starter</a> · <a href="developer/${version}/DeveloperReleaseData.hpp">bootstrap configuration</a></li>`).join('')}</ul></html>\n`);
    return {version: manifest.version, path: relative};
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    const [distribution, site] = process.argv.slice(2);
    if (!distribution || !site) throw new Error('Usage: node tools/developer/site.mjs <distribution> <site-directory>');
    console.log(await stageSite(resolve(distribution), resolve(site)));
}
