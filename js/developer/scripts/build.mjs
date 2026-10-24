// Builds the developer environment, its starters and the pinned header from the sources of this repository, with wf:
//
//   wf run js/developer/scripts/build.mjs [<output directory>] [--refresh-bootstrap]
//
// The compiler is the locked esbuild-wasm of webfront.project.json, run in the script's Worker; the files it reads are
// the project's and the locked packages'. With --refresh-bootstrap, the checked-in bootstrap bundles, their checksums and
// the pinned header are regenerated too; the core headers that embed the bundles come from the CMake target
// webfront-embedded-assets (js/core/EmbedAsset.cmake).
import * as esbuild from 'esbuild-wasm';
import {virtualPlugin} from '../environment/bundler.mjs';
import {compress} from '../environment/embed.mjs';
import {packageRoot} from '../environment/paths.mjs';
import {tar} from '../tools/archive.mjs';
import {baseUrl, versionPath, bootstrapHeader, sriOf} from '../tools/distribution.mjs';

const manifestPath = 'js/developer/webfront.project.json';
const lockPath = 'js/developer/webfront-packages.lock.json';
const encoder = new TextEncoder();
const hexOf = bytes => Array.from(new Uint8Array(bytes), byte => byte.toString(16).padStart(2, '0')).join('');

/** The project path of a file of a locked package: the root of that name. */
export function lockedFile(lock, name, file) {
    const node = lock.packages[lock.roots[name]?.id];
    if (!node) throw new Error(`${name} is not installed: run wf install`);
    return `${packageRoot}/${node.key}/${file}`;
}

export default async function (context) {
    const {project} = context;
    const flags = context.args.filter(argument => argument.startsWith('--'));
    const refresh = flags.includes('--refresh-bootstrap');
    const unknown = flags.find(flag => flag !== '--refresh-bootstrap');
    if (unknown) throw new Error(`Unknown option ${unknown}`);
    const output = context.args.find(argument => !argument.startsWith('--')) ?? 'build-developer-environment';
    const lock = JSON.parse(await project.readText(lockPath));
    const readProject = path => project.read(path);

    await esbuild.initialize({wasmModule: await WebAssembly.compile(await project.read(lockedFile(lock, 'esbuild-wasm', 'esbuild.wasm'))), worker: false});
    const bundle = async entry => {
        const result = await esbuild.build({entryPoints: [entry], bundle: true, format: 'esm', platform: 'browser', target: 'es2020',
            minify: true, legalComments: 'inline', write: false, outfile: '/output/bundle.mjs',
            plugins: [virtualPlugin(lock, undefined, readProject)]});
        return result.outputFiles[0].contents;
    };
    const written = new Map();
    const write = async (path, data) => {
        const bytes = typeof data === 'string' ? encoder.encode(data) : data;
        await project.write(path, bytes);
        written.set(path, bytes);
        return bytes;
    };
    const read = async path => written.get(path) ?? await project.read(path);

    const files = new Map();
    for (const name of ['runtime', 'compiler', 'script-worker', 'typecheck-worker'])
        files.set(`${name}.mjs`, await write(`${output}/${name}.mjs`, await bundle(`js/developer/environment/${name}.mjs`)));
    files.set('esbuild.wasm', await write(`${output}/esbuild.wasm`, await project.read(lockedFile(lock, 'esbuild-wasm', 'esbuild.wasm'))));
    files.set('xterm.css', await write(`${output}/xterm.css`, await project.read(lockedFile(lock, '@xterm/xterm', 'css/xterm.css'))));

    const distribution = JSON.parse(await project.readText('js/developer/tools/distribution.json'));
    const version = distribution.version;
    const publicBase = baseUrl(distribution.baseUrl);
    versionPath(version);
    const dependencies = JSON.parse(await project.readText(manifestPath)).dependencies;
    const licenses = [
        ['esbuild-wasm', lockedFile(lock, 'esbuild-wasm', 'LICENSE.md')],
        ['semver', lockedFile(lock, 'semver', 'LICENSE')],
        ['xterm', lockedFile(lock, '@xterm/xterm', 'LICENSE')],
        ['xterm Fit addon', lockedFile(lock, '@xterm/addon-fit', 'LICENSE')],
        ['fflate', lockedFile(lock, 'fflate', 'LICENSE')],
    ];
    const notices = [];
    for (const [name, path] of licenses) notices.push(`${name}\n${await project.readText(path)}`);
    files.set('LICENSES.txt', await write(`${output}/LICENSES.txt`, notices.join('\n\n')));

    // The starter is pinned by the environment's own trusted archive, not by unverified remote metadata.
    const starters = {};
    for (const [name, provider] of [['life', 'LifeUI']]) {
        const starterFiles = new Map();
        for (const file of ['main.tsx', 'style.css'])
            starterFiles.set(`package/${file}`, await project.read(`examples/life/ui/${file}`));
        const recipe = {format: 1, name: `${name}-starter`, version, dependencies: {
            react: dependencies.react, 'react-dom': dependencies['react-dom']},
            entries: {main: 'main.tsx'}, providers: {[provider]: {entry: 'main', output: `generated/${provider}.hpp`}}};
        starterFiles.set('package/webfront.project.json', encoder.encode(JSON.stringify(recipe, null, 2) + '\n'));
        starterFiles.set('package/package.json', encoder.encode(JSON.stringify({name: `@webfront/${name}-starter`, version,
            private: true, webfront: {kind: 'starter', entry: 'main.tsx'}}, null, 2) + '\n'));
        const starterBytes = compress(tar(starterFiles));
        const archive = `${name}-starter.tgz`;
        await write(`${output}/${archive}`, starterBytes);
        starters[name] = {archive, integrity: await sriOf(starterBytes)};
    }
    files.set('environment.json', await write(`${output}/environment.json`, JSON.stringify({protocol: 1, api: 1, version, entry: 'runtime.mjs',
        compiler: `esbuild-wasm@${dependencies['esbuild-wasm']}`, semver: dependencies.semver, starters}, null, 2) + '\n'));
    files.set('package.json', await write(`${output}/package.json`, JSON.stringify({name: '@webfront/developer-environment', version,
        description: 'Browser-powered WebFront developer environment', type: 'module',
        files: ['runtime.mjs', 'compiler.mjs', 'script-worker.mjs', 'typecheck-worker.mjs', 'esbuild.wasm', 'xterm.css', 'environment.json', 'LICENSES.txt']}, null, 2) + '\n'));

    const bytes = compress(tar(new Map([...files].map(([name, content]) => [`package/${name}`, content]))));
    await write(`${output}/environment.tgz`, bytes);
    const integrity = await sriOf(bytes);
    await write(`${output}/provision.json`, JSON.stringify({format: 1, version, environment: './environment.tgz', integrity, starters}, null, 2) + '\n');
    const header = bootstrapHeader(version, publicBase, integrity);
    await write(`${output}/DeveloperReleaseData.hpp`, header);

    if (refresh) {
        await write('js/core/bootstrap/bootstrap.bundle.mjs', await bundle('js/developer/environment/bootstrap-entry.mjs'));
        await write('js/developer/environment/packages.bundle.mjs', await bundle('js/developer/environment/packages-entry.mjs'));
        await write('js/developer/environment/PACKAGES-LICENSE.txt', await project.read(lockedFile(lock, 'semver', 'LICENSE')));
        await write('include/webfront/system/DeveloperReleaseData.hpp', header);
        const inputs = {};
        for (const path of JSON.parse(await project.readText('js/developer/tools/bundle-inputs.json')))
            inputs[path] = hexOf(await crypto.subtle.digest('SHA-256', await read(path)));
        await write('js/developer/environment/bundles.json', JSON.stringify(inputs, null, 2) + '\n');
    }
    context.console.log(`Environment: ${output}/environment.tgz\nIntegrity: ${integrity}\nStarters: ${Object.keys(starters).join(', ')}`);
}
