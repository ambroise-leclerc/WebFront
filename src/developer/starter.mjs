import {gunzip, readTar, verifyIntegrity} from '../components/tools/wf.mjs';
import {safePackagePath} from './paths.mjs';
import {publish, responseBytes} from './bootstrap.mjs';

const decoder = new TextDecoder();
export function recipeOf(files) {
    const recipe = JSON.parse(decoder.decode(files.get('webfront.project.json')));
    if (recipe.format !== 1 || typeof recipe.entry !== 'string' || !files.has(safePackagePath(recipe.entry)) ||
        !recipe.dependencies || typeof recipe.dependencies !== 'object' || Array.isArray(recipe.dependencies))
        throw new Error('Invalid starter project recipe');
    for (const [name, version] of Object.entries(recipe.dependencies))
        if (!/^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*$/.test(name) ||
            typeof version !== 'string' || !/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version))
            throw new Error('Starter dependencies must name exact npm versions');
    return recipe;
}

export async function createStarter(bridge, config, name, destination = 'ui', signal, fetchImpl = fetch) {
    safePackagePath(destination);
    const starter = config.starters?.[name];
    if (!starter?.url || !starter.integrity?.startsWith('sha512-')) throw new Error(`Unknown pinned starter: ${name}`);
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
    const recipe = recipeOf(files);
    signal?.throwIfAborted();
    // A starter must never overwrite an existing directory, including a symlink or partial project.
    await publish(bridge, destination, files, true, signal, false);
    const install = Object.entries(recipe.dependencies).map(([pkg, version]) => `${pkg}@${version}`).join(' ');
    return `Created ${destination}.\nwf install ${install}\nwf preview ${destination}/${recipe.entry}\nwf embed ${destination}/${recipe.entry} --output generated/PiUI.hpp`;
}
