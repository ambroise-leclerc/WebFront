// The local npm registry of the tests: packuments and tarballs as plain files, generated reproducibly from the pinned real
// archives (test/web/registry-packages.json, downloaded and verified at configure time) and from synthetic packages for
// the edge cases. The test application serves the directory from its own origin, so no test reaches the public registry.
// Everything here uses only browser APIs: build.mjs runs it with wf.
import {tar} from '../../../js/developer/tools/archive.mjs';
import {compress} from '../../../js/developer/environment/embed.mjs';
import {gunzip, readTar} from '../../../js/developer/environment/tarball.mjs';

const encoder = new TextEncoder();
async function integrity(bytes) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-512', bytes));
    return 'sha512-' + btoa(String.fromCharCode(...digest));
}
const decoder = new TextDecoder();
const archive = files => compress(tar(new Map([...files].map(([path, bytes]) => [`package/${path}`, bytes]))));
// The fixture's versions are plain major.minor.patch numbers.
function compareVersions(a, b) {
    const [x, y] = [a, b].map(version => version.split('.').map(Number));
    for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return 0;
}
const newest = versions => versions.toSorted(compareVersions).at(-1);

/** A real archive, kept byte for byte: its manifest is the package.json inside it. */
export async function realPackage(bytes) {
    const entry = readTar(await gunzip(bytes)).find(file => file.path === 'package/package.json');
    if (!entry) throw new Error('An archive of the registry has no package/package.json');
    return {manifest: JSON.parse(decoder.decode(entry.bytes)), bytes};
}

function synthetic(name, version, fields = {}, extra = {}) {
    const manifest = {name, version, type: 'module', main: 'index.js', ...fields};
    return {manifest, files: new Map([
        ['package.json', encoder.encode(JSON.stringify(manifest, null, 2) + '\n')],
        ['index.js', encoder.encode(`export default ${JSON.stringify(`${name}@${version}`)};\n`)],
        ...Object.entries(extra).map(([path, text]) => [path, encoder.encode(text)]),
    ])};
}

// A WebFront executable entry for wf exec: it writes its arguments and its dependency's identity.
const toolEntry = `import lib from 'webfront-fixture-lib';
export default async function (context) {
    await context.project.write('generated/script/tool.txt', [lib, ...context.args].join(' '));
}
`;

// Packages typed for wf check: declarations from the "types" condition of "exports" (where esbuild would take the
// browser build) importing a dependency that is not a root, from "typings", and from @types only; and a typescript
// older than the versions wf check supports.
const typedPackages = () => [
    synthetic('webfront-fixture-typed', '1.0.0', {exports: {'.': {types: './index.d.ts', browser: './browser.js', import: './index.js'}},
        dependencies: {'webfront-fixture-shape': '^1.0.0'}}, {
        'index.d.ts': "import type {Shape} from 'webfront-fixture-shape';\nexport declare const answer: number;\nexport declare function area(shape: Shape): number;\n",
        'browser.js': 'export const answer = 42;\nexport const area = shape => shape.width * shape.height;\n'}),
    synthetic('webfront-fixture-shape', '1.0.0', {types: 'shape.d.ts'}, {'shape.d.ts': 'export interface Shape { width: number; height: number }\n'}),
    synthetic('webfront-fixture-legacy', '1.0.0', {typings: 'types/legacy.d.ts'}, {'types/legacy.d.ts': "export declare const legacy: 'yes';\n"}),
    synthetic('webfront-fixture-untyped', '1.0.0'),
    synthetic('@types/webfront-fixture-untyped', '1.0.0', {types: 'index.d.ts'}, {'index.d.ts': 'export declare function greet(name: string): string;\n'}),
    synthetic('typescript', '4.9.5', {main: 'lib/typescript.js'}, {'lib/typescript.js': 'module.exports = {version: "4.9.5"};\n'}),
];

/**
 * Edge cases: a dependency range under which a newer version than the locked one exists (a restore must not move
 * to it), a published archive that no longer matches the integrity of its packument, a package with a WebFront
 * executable entry and one with only an npm "bin". A package absent from the registry is simply not generated.
 */
export const syntheticPackages = () => [
    synthetic('webfront-fixture-app', '1.0.0', {dependencies: {'webfront-fixture-lib': '^1.0.0'}}),
    synthetic('webfront-fixture-lib', '1.0.0'),
    synthetic('webfront-fixture-lib', '1.1.0'),
    {...synthetic('webfront-fixture-tampered', '1.0.0'), tampered: true},
    synthetic('webfront-fixture-tool', '1.0.0', {dependencies: {'webfront-fixture-lib': '^1.0.0'},
        webfront: {bin: {'webfront-fixture-tool': './bin/tool.mjs'}}}, {'bin/tool.mjs': toolEntry}),
    synthetic('webfront-fixture-nodebin', '1.0.0', {bin: {'webfront-fixture-nodebin': 'cli.js'}}, {'cli.js': 'console.log(1);\n'}),
    ...typedPackages(),
];

function byName(packages) {
    const names = new Map();
    for (const pkg of packages) names.set(pkg.manifest.name, [...names.get(pkg.manifest.name) ?? [], pkg]);
    return names;
}

/**
 * The registry's files: `<name>` packuments (`@scope/<name>` in a scope directory, which the client requests as
 * `@scope%2f<name>`) and `<name>-<version>.tgz` archives (`scope-<name>-<version>.tgz`), and each package's integrity.
 */
export async function registryFiles(packages) {
    const files = new Map();
    const integrities = {};
    for (const [name, versions] of byName(packages)) {
        const described = {};
        for (const {manifest, files: contents, bytes: real, tampered} of versions) {
            const file = `${name.replace(/^@/, '').replace('/', '-')}-${manifest.version}.tgz`;
            const bytes = real ?? archive(contents);
            files.set(file, tampered ? archive(new Map([...contents, ['TAMPERED', encoder.encode('changed')]])) : bytes);
            integrities[`${name}@${manifest.version}`] = await integrity(bytes);
            described[manifest.version] = {...manifest, dist: {tarball: `./${file}`, integrity: integrities[`${name}@${manifest.version}`]}};
        }
        const packument = {name, 'dist-tags': {latest: newest(Object.keys(described))}, versions: described};
        files.set(name, encoder.encode(JSON.stringify(packument, null, 2) + '\n'));
    }
    return {files, integrities};
}
