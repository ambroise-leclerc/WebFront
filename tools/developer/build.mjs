// Maintainer provisioning only. Consumers download the resulting tarball, without Node/npm.
import {build} from '../assets/node_modules/esbuild/lib/main.js';
import {readFile, writeFile, mkdir, copyFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {gzipSync} from '../../third_party/fflate/browser.mjs';
import {tar} from './archive.mjs';
import {baseUrl, versionPath, bootstrapHeader} from './site.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));
const args = process.argv.slice(2);
const refresh = args.includes('--refresh-bootstrap');
const output = resolve(args.find(arg => !arg.startsWith('--')) ?? resolve(root, 'build-developer-environment'));
await mkdir(output, {recursive: true});
const options = {
    absWorkingDir: root, bundle: true, format: 'esm', platform: 'browser', target: 'es2020',
    minify: true, legalComments: 'inline',
    nodePaths: [resolve(root, 'tools/assets/node_modules')],
    plugins: [{name: 'terminal-assets', setup(build) {
        build.onResolve({filter: /^\.\/(xterm|addon-fit)\.mjs$/}, args => ({path: resolve(root, 'third_party/xterm', args.path)}));
    }}],
};
await build({...options, entryPoints: ['assets/developer/runtime.mjs'], outfile: resolve(output, 'runtime.mjs')});
await build({...options, entryPoints: ['assets/developer/compiler.mjs'], outfile: resolve(output, 'compiler.mjs')});
await copyFile(resolve(root, 'tools/assets/node_modules/esbuild-wasm/esbuild.wasm'), resolve(output, 'esbuild.wasm'));
await copyFile(resolve(root, 'third_party/xterm/xterm.css'), resolve(output, 'xterm.css'));
const distribution = JSON.parse(await readFile(resolve(root, 'tools/developer/distribution.json')));
const version = distribution.version;
const publicBase = baseUrl(distribution.baseUrl);
versionPath(version);
const dependencies = JSON.parse(await readFile(resolve(root, 'tools/assets/package.json'))).dependencies;
const licenses = [
    ['esbuild-wasm', 'tools/assets/node_modules/esbuild-wasm/LICENSE.md'],
    ['semver', 'tools/assets/node_modules/semver/LICENSE'],
    ['xterm', 'third_party/xterm/LICENSE'],
    ['xterm Fit addon', 'third_party/xterm/addon-fit-LICENSE'],
    ['fflate', 'third_party/fflate/LICENSE'],
];
const notices = [];
for (const [name, path] of licenses) notices.push(`${name}\n${await readFile(resolve(root, path), 'utf8')}`);
await writeFile(resolve(output, 'LICENSES.txt'), notices.join('\n\n'));
// The starter is pinned by the environment's own trusted archive, not by unverified remote metadata.
const starters = {};
for (const [name, provider] of [['pi', 'PiUI'], ['life', 'LifeUI']]) {
    const starterFiles = new Map();
    for (const file of ['main.tsx', 'style.css'])
        starterFiles.set(`package/${file}`, await readFile(resolve(root, `${name === 'life' ? 'examples/life/ui' : 'tools/developer/starters/pi'}/${file}`)));
    const recipe = {format: 1, name, provider, entry: 'main.tsx', dependencies: {
        react: dependencies.react, 'react-dom': dependencies['react-dom']}};
    starterFiles.set('package/webfront.project.json', Buffer.from(JSON.stringify(recipe, null, 2) + '\n'));
    starterFiles.set('package/package.json', Buffer.from(JSON.stringify({name: `@webfront/${name}-starter`, version,
        private: true, webfront: {kind: 'starter', entry: 'main.tsx'}}, null, 2) + '\n'));
    const starterBytes = gzipSync(tar(starterFiles), {level: 9, mem: 8, mtime: 0});
    const archive = `${name}-starter.tgz`;
    await writeFile(resolve(output, archive), starterBytes);
    starters[name] = {archive, integrity: 'sha512-' + createHash('sha512').update(starterBytes).digest('base64')};
}
await writeFile(resolve(output, 'environment.json'), JSON.stringify({protocol: 1, version, entry: 'runtime.mjs',
    compiler: `esbuild-wasm@${dependencies['esbuild-wasm']}`, semver: dependencies.semver, starters}, null, 2) + '\n');
await writeFile(resolve(output, 'package.json'), JSON.stringify({name: '@webfront/developer-environment', version,
    description: 'Browser-powered WebFront developer environment', type: 'module',
    files: ['runtime.mjs', 'compiler.mjs', 'esbuild.wasm', 'xterm.css', 'environment.json', 'LICENSES.txt']}, null, 2) + '\n');
const files = new Map();
for (const name of ['runtime.mjs', 'compiler.mjs', 'esbuild.wasm', 'xterm.css', 'environment.json', 'LICENSES.txt', 'package.json'])
    files.set(`package/${name}`, await readFile(resolve(output, name)));
const bytes = gzipSync(tar(files), {level: 9, mem: 8, mtime: 0});
await writeFile(resolve(output, 'environment.tgz'), bytes);
const integrity = 'sha512-' + createHash('sha512').update(bytes).digest('base64');
await writeFile(resolve(output, 'provision.json'), JSON.stringify({format: 1, version, environment: './environment.tgz', integrity, starters}, null, 2) + '\n');
const header = bootstrapHeader(version, publicBase, integrity);
await writeFile(resolve(output, 'DeveloperReleaseData.hpp'), header);
if (refresh) {
    const bootstrap = resolve(root, 'assets/developer/bootstrap.bundle.mjs');
    await build({...options, entryPoints: ['assets/developer/bootstrap-entry.mjs'], outfile: bootstrap});
    await build({...options, entryPoints: ['assets/developer/packages.mjs'], outfile: resolve(root, 'assets/developer/packages.bundle.mjs')});
    await copyFile(resolve(root, 'tools/assets/node_modules/semver/LICENSE'), resolve(root, 'assets/developer/PACKAGES-LICENSE.txt'));
    await writeFile(resolve(root, 'include/system/DeveloperReleaseData.hpp'), header);
    const inputs = {};
    for (const path of ['assets/developer/bootstrap.mjs', 'assets/developer/bootstrap-entry.mjs', 'assets/developer/packages.mjs', 'assets/developer/starter.mjs', 'assets/developer/paths.mjs', 'assets/developer/runtime.mjs', 'assets/developer/compiler.mjs', 'assets/developer/bundler.mjs', 'assets/developer/entry.mjs', 'assets/developer/conditions.mjs', 'assets/developer/io.mjs', 'assets/developer/recipe.mjs', 'assets/developer/registry.mjs', 'assets/developer/archive.mjs', 'assets/developer/lock.mjs', 'assets/developer/pins.mjs', 'assets/developer/peers.mjs', 'assets/developer/resolve.mjs', 'assets/developer/embed.mjs', 'tools/developer/starters/pi/main.tsx', 'tools/developer/starters/pi/style.css', 'examples/life/ui/main.tsx', 'examples/life/ui/style.css', 'assets/components/tools/wf.mjs', 'tools/developer/distribution.json', 'tools/developer/build.mjs', 'tools/developer/site.mjs', 'tools/developer/archive.mjs', 'tools/assets/package-lock.json', 'assets/developer/bootstrap.bundle.mjs', 'assets/developer/packages.bundle.mjs', 'include/system/DeveloperReleaseData.hpp'])
        inputs[path] = createHash('sha256').update(await readFile(resolve(root, path))).digest('hex');
    await writeFile(resolve(root, 'assets/developer/bundles.json'), JSON.stringify(inputs, null, 2) + '\n');
    for (const [source, name] of [['assets/developer/index.html', 'DeveloperIndexHtmlData'], ['assets/developer/bootstrap.bundle.mjs', 'DeveloperBootstrapData']])
        execFileSync('cmake', [`-DINPUT=${resolve(root, source)}`, `-DOUTPUT=${resolve(root, 'include/system', name + '.hpp')}`, `-DSTRUCT=${name}`, '-P', resolve(root, 'cmake/EmbedTextAsset.cmake')]);
}
console.log(`Environment: ${output}/environment.tgz\nIntegrity: ${integrity}\nStarters: ${Object.keys(starters).join(', ')}`);
