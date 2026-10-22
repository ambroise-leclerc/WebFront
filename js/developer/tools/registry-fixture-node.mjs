// Node-only part of the registry fixture: packages read from an npm ci installation, files written to a directory.
import {readFile, writeFile, mkdir, readdir} from 'node:fs/promises';
import {resolve, dirname} from 'node:path';
import {registryFiles} from './registry-fixture.mjs';

async function collect(directory, files, path = '') {
    const entries = await readdir(resolve(directory, path), {withFileTypes: true});
    for (const entry of entries) {
        const relative = path ? `${path}/${entry.name}` : entry.name;
        if (entry.isDirectory() && entry.name !== 'node_modules') await collect(directory, files, relative);
        else if (entry.isFile()) files.set(relative, await readFile(resolve(directory, relative)));
    }
    return files;
}

/** A package installed by npm ci, as the files of its archive. */
export async function installedPackage(nodeModules, name) {
    const directory = resolve(nodeModules, name);
    return {manifest: JSON.parse(await readFile(resolve(directory, 'package.json'), 'utf8')), files: await collect(directory, new Map())};
}

/** Writes the registry's files into `directory`; returns each package's integrity. */
export async function writeRegistry(directory, packages) {
    const {files, integrities} = await registryFiles(packages);
    await mkdir(directory, {recursive: true});
    for (const [name, bytes] of files) {
        await mkdir(dirname(resolve(directory, name)), {recursive: true});
        await writeFile(resolve(directory, name), bytes);
    }
    return integrities;
}
