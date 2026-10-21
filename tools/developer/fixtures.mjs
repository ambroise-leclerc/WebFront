// Configure offline browser fixtures from the maintainer's pinned npm ci installation.
import {readFile, writeFile, mkdir, readdir, copyFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {tar} from './archive.mjs';
import {compress} from '../../assets/developer/embed.mjs';
const root = fileURLToPath(new URL('../../', import.meta.url));
const output = resolve(process.argv[2] ?? 'build-developer-environment');
await mkdir(resolve(output, 'registry'), {recursive: true});
for (const name of ['react', 'react-dom', 'scheduler']) {
    const directory = resolve(root, 'tools/assets/node_modules', name);
    const manifest = JSON.parse(await readFile(resolve(directory, 'package.json')));
    const files = new Map();
    async function walk(path = '') {
        for (const entry of await readdir(resolve(directory, path), {withFileTypes: true})) {
            if (entry.name === 'node_modules') continue;
            const relative = path ? `${path}/${entry.name}` : entry.name;
            if (entry.isDirectory()) await walk(relative);
            else if (entry.isFile()) files.set(`package/${relative}`, await readFile(resolve(directory, relative)));
        }
    }
    await walk();
    const bytes = compress(tar(files));
    await writeFile(resolve(output, 'registry', `${name}.tgz`), bytes);
    const integrity = 'sha512-' + createHash('sha512').update(bytes).digest('base64');
    await writeFile(resolve(output, 'registry', name), JSON.stringify({'dist-tags': {latest: manifest.version},
        versions: {[manifest.version]: {...manifest, dist: {tarball: `./${name}.tgz`, integrity}}}}));
}
await copyFile(resolve(root, 'webtest/developer-specs.mjs'), resolve(output, 'run.mjs'));
await copyFile(resolve(root, 'tools/developer/starters/pi/main.tsx'), resolve(output, 'main.tsx'));
await copyFile(resolve(root, 'tools/developer/starters/pi/style.css'), resolve(output, 'style.css'));
console.log(`Offline developer fixtures: ${output}`);
