// A starter's webfront.project.json: the project manifest it ships, with exact dependency versions.
import {safePackagePath} from './paths.mjs';
import {parseManifest, manifestFileName, exactVersion, npmName} from './manifest.mjs';

function requireExactDependencies(dependencies) {
    for (const [name, version] of Object.entries(dependencies))
        if (!npmName.test(name) || !exactVersion.test(version))
            throw new Error('Starter dependencies must name exact npm versions');
}

// A starter has a main entry present in its files and, optionally, one provider.
export function recipeOf(files) {
    if (!files.has(manifestFileName)) throw new Error('Invalid starter project recipe');
    const manifest = parseManifest(files.get(manifestFileName));
    if (!Object.hasOwn(manifest.entries, 'main') || !files.has(safePackagePath(manifest.entries.main)))
        throw new Error('Invalid starter project recipe');
    requireExactDependencies(manifest.dependencies);
    const [provider] = Object.keys(manifest.providers);
    return {...manifest, entry: manifest.entries.main, provider, output: manifest.providers[provider]?.output};
}
