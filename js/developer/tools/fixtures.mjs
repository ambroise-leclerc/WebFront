// Configure offline browser fixtures from the maintainer's pinned npm ci installation.
import {mkdir, copyFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {pinnedPackages, syntheticPackages} from './registry-fixture.mjs';
import {installedPackage, writeRegistry} from './registry-fixture-node.mjs';
const root = fileURLToPath(new URL('../../../', import.meta.url));
const output = resolve(process.argv[2] ?? 'build-developer-environment');
await mkdir(output, {recursive: true});
const nodeModules = resolve(root, 'js/developer/vendor/node_modules');
const pinned = await Promise.all(pinnedPackages.map(name => installedPackage(nodeModules, name)));
await writeRegistry(resolve(output, 'registry'), [...pinned, ...syntheticPackages()]);
await copyFile(resolve(root, 'test/web/developer-specs.mjs'), resolve(output, 'run.mjs'));
await copyFile(resolve(root, 'test/web/developer-session.html'), resolve(output, 'session.html'));
await copyFile(resolve(root, 'test/web/developer-session.mjs'), resolve(output, 'session.mjs'));
await copyFile(resolve(root, 'js/developer/tools/starters/pi/main.tsx'), resolve(output, 'main.tsx'));
await copyFile(resolve(root, 'js/developer/tools/starters/pi/style.css'), resolve(output, 'style.css'));
console.log(`Offline developer fixtures: ${output}`);
