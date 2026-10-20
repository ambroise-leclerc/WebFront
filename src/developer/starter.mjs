import {gunzip, readTar, verifyIntegrity} from '../components/tools/wf.mjs';
import {safePackagePath} from './paths.mjs';
import {publish, responseBytes} from './bootstrap.mjs';

const decoder = new TextDecoder();
const npmName = /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/;
const exactVersion = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const isObject = value => Boolean(value) && typeof value === 'object' && !Array.isArray(value);

function requireExactDependencies(dependencies) {
    if (!isObject(dependencies)) throw new Error('Invalid starter project recipe');
    for (const [name, version] of Object.entries(dependencies))
        if (!npmName.test(name) || typeof version !== 'string' || !exactVersion.test(version))
            throw new Error('Starter dependencies must name exact npm versions');
}

function validProvider(provider) {
    if (typeof provider !== 'string') return false;
    return /^[A-Za-z_][A-Za-z0-9_]*$/.test(provider);
}

export function recipeOf(files) {
    const recipe = JSON.parse(decoder.decode(files.get('webfront.project.json')));
    if (recipe.format !== 1 || typeof recipe.entry !== 'string' || !files.has(safePackagePath(recipe.entry)))
        throw new Error('Invalid starter project recipe');
    requireExactDependencies(recipe.dependencies);
    if (recipe.provider !== undefined && !validProvider(recipe.provider))
        throw new Error('Invalid starter provider name');
    return recipe;
}

async function downloadStarter(starter, {signal, fetchImpl}) {
    if (!starter?.url || !starter.integrity?.startsWith('sha512-')) throw new Error('Unknown pinned starter');
    const url = new URL(starter.url, location.href);
    if (url.protocol !== 'https:' && url.origin !== location.origin) throw new Error('Starter requires HTTPS or the application origin');
    const bytes = await responseBytes(await fetchImpl(url, {signal}));
    await verifyIntegrity(bytes, starter.integrity);
    const files = new Map();
    for (const entry of readTar(await gunzip(bytes))) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid starter archive root');
        const path = safePackagePath(entry.path.slice(8));
        if (files.has(path)) throw new Error('Duplicate starter file');
        files.set(path, entry.bytes);
    }
    return files;
}

export async function createStarter(bridge, config, [name, destination = 'ui'], {signal, fetchImpl = fetch} = {}) {
    safePackagePath(destination);
    const starter = config.starters?.[name];
    if (!starter) throw new Error(`Unknown pinned starter: ${name}`);
    const files = await downloadStarter(starter, {signal, fetchImpl});
    const recipe = recipeOf(files);
    signal?.throwIfAborted();
    // A starter must never overwrite an existing directory, including a symlink or partial project.
    await publish(bridge, destination, files, {signal, replace: false});
    const install = Object.entries(recipe.dependencies).map(([pkg, version]) => `${pkg}@${version}`).join(' ');
    return `Created ${destination}.\nwf install ${install}\nwf preview ${destination}/${recipe.entry}\nwf embed ${destination}/${recipe.entry} --output generated/${recipe.provider ?? 'PiUI'}.hpp`;
}
