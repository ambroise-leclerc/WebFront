import {gunzip, readTar, verifyIntegrity} from './tarball.mjs';
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

/** The starters this environment pins, sorted: the only values `wf create` accepts. */
export const starterNames = config => Object.keys(config?.starters ?? {}).sort();

// The argument of wf create is easily taken for the name of the project to create.
const unknownStarter = (name, config) => `Unknown pinned starter: ${name}. wf create takes the name of a starter, not of your project` +
    (starterNames(config).length ? `; available: ${starterNames(config).join(', ')}` : '');

export async function createStarter(bridge, config, [name, destination = 'ui'], {signal, fetchImpl = fetch} = {}) {
    safePackagePath(destination);
    const starter = Object.hasOwn(config.starters ?? {}, name) ? config.starters[name] : undefined;
    if (!starter) throw new Error(unknownStarter(name, config));
    const files = await downloadStarter(starter, {signal, fetchImpl});
    const recipe = recipeOf(files);
    signal?.throwIfAborted();
    // A starter must never overwrite an existing directory, including a symlink or partial project.
    await publish(bridge, destination, files, {signal, replace: false});
    const install = Object.entries(recipe.dependencies).map(([pkg, version]) => `${pkg}@${version}`).join(' ');
    const output = recipe.output ?? `generated/${recipe.provider ?? 'AppUI'}.hpp`;
    const entry = `${destination}/${recipe.entry}`;
    // The next commands, in order; a starter without dependency has nothing to install.
    return [`Created ${destination}.`, 'wf init', ...(install ? [`wf install ${install}`] : []), `wf dev ${entry}`,
        `wf preview ${entry}`, `wf embed ${entry} --output ${output}`].join('\n');
}
