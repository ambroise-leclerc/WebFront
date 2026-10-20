// Maintainer provisioning only. Consumers download the resulting tarball, without Node/npm.
import {build} from '../assets/node_modules/esbuild/lib/main.js';
import {readFile, writeFile, mkdir, copyFile} from 'node:fs/promises';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {gzipSync} from '../../third_party/fflate/browser.mjs';
import {tar} from './archive.mjs';

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
if (refresh) {
    const bootstrap = resolve(root, 'src/developer/bootstrap.bundle.mjs');
    await build({...options, entryPoints: ['src/developer/bootstrap-entry.mjs'], outfile: bootstrap});
    await build({...options, entryPoints: ['src/developer/packages.mjs'], outfile: resolve(root, 'src/developer/packages.bundle.mjs')});
    await copyFile(resolve(root, 'tools/assets/node_modules/semver/LICENSE'), resolve(root, 'src/developer/PACKAGES-LICENSE.txt'));
    const inputs = {};
    for (const path of ['src/developer/bootstrap.mjs', 'src/developer/bootstrap-entry.mjs', 'src/developer/packages.mjs', 'src/components/tools/wf.mjs', 'tools/assets/package-lock.json', 'src/developer/bootstrap.bundle.mjs', 'src/developer/packages.bundle.mjs'])
        inputs[path] = createHash('sha256').update(await readFile(resolve(root, path))).digest('hex');
    await writeFile(resolve(root, 'src/developer/bundles.json'), JSON.stringify(inputs, null, 2) + '\n');
    for (const [source, name] of [['src/developer/index.html', 'DeveloperIndexHtmlData'], ['src/developer/bootstrap.bundle.mjs', 'DeveloperBootstrapData']])
        execFileSync('cmake', [`-DINPUT=${resolve(root, source)}`, `-DOUTPUT=${resolve(root, 'include/system', name + '.hpp')}`, `-DSTRUCT=${name}`, '-P', resolve(root, 'cmake/EmbedTextAsset.cmake')]);
}
await build({...options, entryPoints: ['src/developer/runtime.mjs'], outfile: resolve(output, 'runtime.mjs')});
await build({...options, entryPoints: ['src/developer/compiler.mjs'], outfile: resolve(output, 'compiler.mjs')});
await copyFile(resolve(root, 'tools/assets/node_modules/esbuild-wasm/esbuild.wasm'), resolve(output, 'esbuild.wasm'));
await copyFile(resolve(root, 'third_party/xterm/xterm.css'), resolve(output, 'xterm.css'));
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
await writeFile(resolve(output, 'environment.json'), JSON.stringify({protocol: 1, entry: 'runtime.mjs',
    compiler: `esbuild-wasm@${dependencies['esbuild-wasm']}`, semver: dependencies.semver}, null, 2) + '\n');
await writeFile(resolve(output, 'package.json'), JSON.stringify({name: '@webfront/developer-environment', version: '0.1.0',
    description: 'Browser-powered WebFront developer environment', type: 'module',
    files: ['runtime.mjs', 'compiler.mjs', 'esbuild.wasm', 'xterm.css', 'environment.json', 'LICENSES.txt']}, null, 2) + '\n');
const files = new Map();
for (const name of ['runtime.mjs', 'compiler.mjs', 'esbuild.wasm', 'xterm.css', 'environment.json', 'LICENSES.txt', 'package.json'])
    files.set(`package/${name}`, await readFile(resolve(output, name)));
const bytes = gzipSync(tar(files), {level: 9, mem: 8, mtime: 0});
await writeFile(resolve(output, 'environment.tgz'), bytes);
const integrity = 'sha512-' + createHash('sha512').update(bytes).digest('base64');
await writeFile(resolve(output, 'provision.json'), JSON.stringify({environment: './environment.tgz', integrity}, null, 2) + '\n');
console.log(`Environment: ${output}/environment.tgz\nIntegrity: ${integrity}`);
