// Reading an npm package archive: its files and the checks on its package.json.
import {gunzip, readTar} from './tarball.mjs';
import {safePackagePath} from './paths.mjs';

const text = new TextDecoder();
const installScripts = ['preinstall', 'install', 'postinstall'];

export async function readArchive(bytes) {
    const files = new Map();
    for (const entry of readTar(await gunzip(bytes))) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid npm tarball root');
        const path = safePackagePath(entry.path.slice(8));
        if (files.has(path)) throw new Error(`Duplicate package path: ${path}`);
        files.set(path, entry.bytes);
    }
    return files;
}

const hasInstallScript = pkg => installScripts.some(key => pkg.scripts?.[key]);
const hasNativeFile = files => files.has('binding.gyp') || [...files.keys()].some(path => path.endsWith('.node'));

function needsInstallStep(pkg, files) {
    if (hasInstallScript(pkg)) return true;
    if (pkg.gypfile) return true;
    return hasNativeFile(files);
}

// The package.json inside the archive must be the requested package and need no install step.
export function checkedManifest(files, name, version) {
    const pkg = JSON.parse(text.decode(files.get('package.json')));
    if (pkg.name !== name || pkg.version !== version) throw new Error('npm archive identity mismatch');
    if (needsInstallStep(pkg, files)) throw new Error(`${name} needs an unsupported install script or native module`);
    return pkg;
}
