// Static, version-preserving GitHub Pages distribution. No dependency on npm at consumption time.
import {readFile, writeFile, mkdir, readdir, stat} from 'node:fs/promises';
import {resolve, join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';
import {baseUrl, versionName, versionPath, bootstrapHeader, distributionFiles} from './distribution.mjs';
export {baseUrl, versionPath, bootstrapHeader, distributionFiles};

const digest = bytes => 'sha512-' + createHash('sha512').update(bytes).digest('base64');

async function readDistribution(distribution) {
    const manifest = JSON.parse(await readFile(join(distribution, 'provision.json')));
    const input = new Map();
    for (const name of distributionFiles(manifest)) input.set(name, await readFile(join(distribution, name)));
    const archives = [[input.get('environment.tgz'), manifest.integrity],
        ...Object.values(manifest.starters).map(starter => [input.get(starter.archive), starter.integrity])];
    if (archives.some(([bytes, integrity]) => digest(bytes) !== integrity)) throw new Error('Distribution archive checksum mismatch');
    return {manifest, input};
}

async function isDirectory(path) {
    try { return (await stat(path)).isDirectory(); }
    catch (error) {
        if (error.code !== 'ENOENT') throw error;
        return false;
    }
}

// A published version is immutable: reruns may only repeat identical bytes.
async function publishVersion(input, destination, version) {
    if (!await isDirectory(destination)) {
        await mkdir(destination, {recursive: true});
        for (const [name, bytes] of input) await writeFile(join(destination, name), bytes);
        return;
    }
    // Check every byte before modifying anything.
    for (const [name, bytes] of input)
        if (!bytes.equals(await readFile(join(destination, name))))
            throw new Error(`Published version ${version} differs; increment the distribution version`);
}

async function versionRow(site, version) {
    const manifest = JSON.parse(await readFile(join(site, versionPath(version), 'provision.json')));
    const links = distributionFiles(manifest).map(name => `<a href="developer/${version}/${name}">${name}</a>`).join(' · ');
    return `<li>${version}: ${links}</li>`;
}

async function writeIndex(site) {
    const versions = (await readdir(join(site, 'developer'))).filter(name => versionName.test(name)).sort();
    await writeFile(join(site, '.nojekyll'), '');
    const rows = [];
    for (const version of versions) rows.push(await versionRow(site, version));
    await writeFile(join(site, 'index.html'), `<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>WebFront developer packages</title><h1>WebFront developer packages</h1><p>Use a version-pinned bootstrap header and SHA-512. React and its dependencies are downloaded separately from npm.</p><ul>${rows.join('')}</ul></html>\n`);
}

export async function stageSite(distribution, site) {
    const {manifest, input} = await readDistribution(distribution);
    const relative = versionPath(manifest.version);
    await publishVersion(input, join(site, relative), manifest.version);
    await writeIndex(site);
    return {version: manifest.version, path: relative};
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    const [distribution, site] = process.argv.slice(2);
    if (!distribution || !site) throw new Error('Usage: node js/developer/tools/site.mjs <distribution> <site-directory>');
    console.log(await stageSite(resolve(distribution), resolve(site)));
}
