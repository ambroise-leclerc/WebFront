// The local npm registry of the tests: packuments and tarballs as plain files, generated reproducibly from pinned
// versions (the maintainer's npm ci installation) and from synthetic packages for the edge cases. The test
// application serves the directory from its own origin, so no test reaches the public registry.
// registryFiles() and syntheticPackages() use only browser APIs, so that wf can generate the fixture itself;
// reading node_modules and writing the directory are the Node-only part.
import {tar} from './archive.mjs';
import {compress} from '../environment/embed.mjs';

/** Pinned packages copied from js/developer/vendor/node_modules, at the versions of its package-lock.json. */
export const pinnedPackages = ['jasmine-core', 'react', 'react-dom', 'scheduler'];

const encoder = new TextEncoder();
async function integrity(bytes) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-512', bytes));
    return 'sha512-' + btoa(String.fromCharCode(...digest));
}
const archive = files => compress(tar(new Map([...files].map(([path, bytes]) => [`package/${path}`, bytes]))));
// The fixture's versions are plain major.minor.patch numbers.
function compareVersions(a, b) {
    const [x, y] = [a, b].map(version => version.split('.').map(Number));
    for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return 0;
}
const newest = versions => versions.toSorted(compareVersions).at(-1);

function synthetic(name, version, fields = {}) {
    const manifest = {name, version, type: 'module', main: 'index.js', ...fields};
    return {manifest, files: new Map([
        ['package.json', encoder.encode(JSON.stringify(manifest, null, 2) + '\n')],
        ['index.js', encoder.encode(`export default ${JSON.stringify(`${name}@${version}`)};\n`)],
    ])};
}

/**
 * Edge cases: a dependency range under which a newer version than the locked one exists (a restore must not move
 * to it), and a published archive that no longer matches the integrity of its packument. A package absent from the
 * registry is simply not generated.
 */
export const syntheticPackages = () => [
    synthetic('webfront-fixture-app', '1.0.0', {dependencies: {'webfront-fixture-lib': '^1.0.0'}}),
    synthetic('webfront-fixture-lib', '1.0.0'),
    synthetic('webfront-fixture-lib', '1.1.0'),
    {...synthetic('webfront-fixture-tampered', '1.0.0'), tampered: true},
];

function byName(packages) {
    const names = new Map();
    for (const pkg of packages) {
        if (pkg.manifest.name.includes('/')) throw new Error('Scoped packages are not supported by the registry fixture');
        names.set(pkg.manifest.name, [...names.get(pkg.manifest.name) ?? [], pkg]);
    }
    return names;
}

/** The registry's files: `<name>` packuments and `<name>-<version>.tgz` archives, and each package's integrity. */
export async function registryFiles(packages) {
    const files = new Map();
    const integrities = {};
    for (const [name, versions] of byName(packages)) {
        const described = {};
        for (const {manifest, files: contents, tampered} of versions) {
            const file = `${name}-${manifest.version}.tgz`;
            const bytes = archive(contents);
            files.set(file, tampered ? archive(new Map([...contents, ['TAMPERED', encoder.encode('changed')]])) : bytes);
            integrities[`${name}@${manifest.version}`] = await integrity(bytes);
            described[manifest.version] = {...manifest, dist: {tarball: `./${file}`, integrity: integrities[`${name}@${manifest.version}`]}};
        }
        const packument = {name, 'dist-tags': {latest: newest(Object.keys(described))}, versions: described};
        files.set(name, encoder.encode(JSON.stringify(packument, null, 2) + '\n'));
    }
    return {files, integrities};
}
