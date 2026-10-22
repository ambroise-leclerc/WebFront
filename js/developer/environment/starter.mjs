import {gunzip, readTar, verifyIntegrity} from '../components/tools/wf.mjs';
import {safePackagePath} from './paths.mjs';
import {publish, responseBytes} from './io.mjs';
import {recipeOf} from './recipe.mjs';
export {recipeOf};

function starterUrl(starter) {
    if (!starter?.url || !starter.integrity?.startsWith('sha512-')) throw new Error('Unknown pinned starter');
    const url = new URL(starter.url, location.href);
    if (url.protocol !== 'https:' && url.origin !== location.origin) throw new Error('Starter requires HTTPS or the application origin');
    return url;
}

async function unpackStarter(bytes) {
    const files = new Map();
    for (const entry of readTar(await gunzip(bytes))) {
        if (!entry.path.startsWith('package/')) throw new Error('Invalid starter archive root');
        const path = safePackagePath(entry.path.slice(8));
        if (files.has(path)) throw new Error('Duplicate starter file');
        files.set(path, entry.bytes);
    }
    return files;
}

async function downloadStarter(starter, {signal, fetchImpl}) {
    const bytes = await responseBytes(await fetchImpl(starterUrl(starter), {signal}));
    await verifyIntegrity(bytes, starter.integrity);
    return unpackStarter(bytes);
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
    const output = recipe.output ?? `generated/${recipe.provider ?? 'PiUI'}.hpp`;
    return `Created ${destination}.\nwf init\nwf install ${install}\nwf preview ${destination}/${recipe.entry}\nwf embed ${destination}/${recipe.entry} --output ${output}`;
}
