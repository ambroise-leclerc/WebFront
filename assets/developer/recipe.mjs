// A starter's webfront.project.json: the entry file, exact dependency versions and the provider name.
import {safePackagePath} from './paths.mjs';
import {isPlainObject} from './entry.mjs';

const decoder = new TextDecoder();
const npmName = /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/;
const exactVersion = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;

function requireExactDependencies(dependencies) {
    if (!isPlainObject(dependencies)) throw new Error('Invalid starter project recipe');
    for (const [name, version] of Object.entries(dependencies))
        if (!npmName.test(name) || typeof version !== 'string' || !exactVersion.test(version))
            throw new Error('Starter dependencies must name exact npm versions');
}

function validProvider(provider) {
    if (typeof provider !== 'string') return false;
    return /^[A-Za-z_][A-Za-z0-9_]*$/.test(provider);
}

function hasEntry(recipe, files) {
    if (typeof recipe.entry !== 'string') return false;
    return files.has(safePackagePath(recipe.entry));
}

export function recipeOf(files) {
    const recipe = JSON.parse(decoder.decode(files.get('webfront.project.json')));
    if (recipe.format !== 1 || !hasEntry(recipe, files)) throw new Error('Invalid starter project recipe');
    requireExactDependencies(recipe.dependencies);
    if (recipe.provider !== undefined && !validProvider(recipe.provider))
        throw new Error('Invalid starter provider name');
    return recipe;
}
