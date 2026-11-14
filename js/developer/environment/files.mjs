// The terminal's file commands: ls, cat, tree and mounts. They only read the project directory, through the callbacks
// that wf uses; paths are relative to the project and never leave it. They are not a shell: no working directory, no
// glob, no pipe, and nothing is written (ADR-023).
import {call, blockSize} from './io.mjs';
import {isUnsafe} from './paths.mjs';
import {manifestPath, lockPath} from './manifest.mjs';
import {printable, inline} from './terminal.mjs';

const decoder = new TextDecoder();
const usage = {
    ls: 'ls [-l] [-a] [--] [path]', cat: 'cat [--] <file> [file...]', tree: 'tree [-a] [-L <depth>] [--] [path]', mounts: 'mounts'};
/** One line per file command, for the terminal's help. */
export const fileCommandHelp = {
    ls: 'List a project directory; -l adds size, date and what wf knows of each entry, -a the hidden entries',
    cat: 'Print text files of the project',
    tree: 'Show a project directory as a tree (3 levels; -L <depth>, -a for the hidden entries)',
    mounts: 'Describe what the application serves to the browser, and from where',
};

const usageError = name => new Error(`Usage: ${usage[name]}`);

/** A project path as the callbacks take it: '' for the project itself, no "./" prefix, no final "/". */
export function projectPath(argument = '') {
    let path = argument;
    while (path.startsWith('./')) path = path.slice(2);
    path = path.replace(/\/+$/, '');
    if (path === '' && argument.startsWith('/')) throw new Error(`${argument}: paths are relative to the project directory`);
    if (path === '' || path === '.') return '';
    if (path.startsWith('/') || isUnsafe(path)) throw new Error(`${argument}: paths are relative to the project directory and stay inside it`);
    return path;
}

// Splits arguments into single-letter flags (-la is -l -a) and the other words. A `valued` flag takes the rest of its
// word (-L3) or the next word (-L 3, -aL 3). After "--" every word is a path, so that a name may start with "-".
function parseArguments(name, args, allowed, valued = '') {
    const flags = new Map(), words = [];
    for (let index = 0; index < args.length; ++index) {
        const word = args[index];
        if (word === '--') { words.push(...args.slice(index + 1)); break; }
        if (!word.startsWith('-') || word === '-') { words.push(word); continue; }
        for (let position = 1; position < word.length; ++position) {
            const flag = word[position];
            if (!allowed.includes(flag)) throw usageError(name);
            if (!valued.includes(flag)) { flags.set(flag, true); continue; }
            const attached = word.slice(position + 1);
            if (!attached && index + 1 === args.length) throw usageError(name);
            flags.set(flag, attached || args[++index]);
            break;
        }
    }
    return {flags, words};
}

const join = (directory, name) => directory ? `${directory}/${name}` : name;
const listNames = async (bridge, directory) => JSON.parse(decoder.decode(await call(bridge, 'list', directory)));
const visible = (names, all) => all ? names : names.filter(name => !name.startsWith('.'));

// What wf keeps under .webfront/: the longest prefix describes an entry.
const managed = [
    ['.webfront/developer/packages', 'package cache (wf install, wf restore)'],
    ['.webfront/developer/environment', 'developer environment, served to the browser'],
    ['.webfront/developer/preview', 'interface built by wf dev and wf preview, served to the browser'],
    ['.webfront/developer/test', 'files of a wf test run, served to the browser'],
    ['.webfront/developer/build', 'output of wf build'],
    ['.webfront/developer/staging', 'publications in progress'],
    ['.webfront/developer/history.json', 'terminal history'],
    ['.webfront', 'managed by wf'],
];
const firstWord = text => String(text).trim().split(/\s+/)[0];

/**
 * What wf knows of a project path: the manifest, the lock, what it manages under .webfront/, and the entries, specs,
 * scripts and generated providers that the manifest declares. An empty string for the project's other files.
 */
export function nature(path, manifest = null) {
    if (path === manifestPath) return 'manifest';
    if (path === lockPath) return 'lock';
    const kept = managed.find(([prefix]) => path === prefix || path.startsWith(prefix + '/'));
    if (kept) return kept[1];
    const named = (section, value, label) => Object.entries(Object(manifest?.[section] ?? {}))
        .filter(([, declared]) => value(declared) === path).map(([name]) => `${label} ${name}`);
    const roles = [
        ...named('entries', entry => entry, 'entry'),
        ...named('providers', provider => provider?.output, 'generated provider'),
        ...named('scripts', script => firstWord(script) === 'wf' ? null : firstWord(script), 'script'),
        ...(Array.isArray(manifest?.test?.specs) && manifest.test.specs.includes(path) ? ['spec'] : []),
    ];
    return roles.join(', ');
}

/** Names laid out in columns read downwards, for a terminal `width` cells wide. */
export function columns(names, width = 80) {
    if (!names.length) return '';
    const cell = Math.max(...names.map(name => name.length)) + 2;
    const rows = Math.ceil(names.length / Math.max(1, Math.floor(width / cell)));
    return Array.from({length: rows}, (_, row) =>
        names.filter((_, index) => index % rows === row).map(name => name.padEnd(cell)).join('').trimEnd()).join('\n');
}

const two = number => String(number).padStart(2, '0');
function timestamp(milliseconds) {
    if (!milliseconds) return '-'.padEnd(16);
    const date = new Date(milliseconds);
    return `${date.getFullYear()}-${two(date.getMonth() + 1)}-${two(date.getDate())} ${two(date.getHours())}:${two(date.getMinutes())}`;
}

/** The lines of ls -l: type, size in bytes, modification time, name, and the entry's nature when wf knows one. */
export function longListing(entries, directory, manifest) {
    const width = Math.max(1, ...entries.map(entry => String(entry.size).length));
    return entries.map(entry => {
        const isDirectory = entry.type === 'directory';
        const role = nature(join(directory, entry.name), manifest);
        const size = (isDirectory ? '-' : String(entry.size)).padStart(width);
        return `${isDirectory ? 'd' : '-'} ${size}  ${timestamp(entry.modified)}  ${inline(entry.name)}${isDirectory ? '/' : ''}${role ? `  [${inline(role)}]` : ''}`;
    }).join('\n');
}

/** Whether an error of the bridge says that the application has no such webfront.developer.* callback (an older library). */
export const isMissingCallback = (error, name) => String(error?.message).includes(`C++ function 'webfront.developer.${name}' was not found`);

async function describedEntries(bridge, directory) {
    try { return JSON.parse(decoder.decode(await call(bridge, 'entries', directory))); }
    catch (error) {
        if (isMissingCallback(error, 'entries'))
            throw new Error('ls -l needs a more recent WebFront library in the application; ls and tree work with this one');
        throw error;
    }
}

// A path that is not a directory is listed as the one entry of its directory, when it is there.
async function listing(bridge, path, read) {
    try { return {directory: path, items: await read(path)}; }
    catch (error) {
        if (!path || !/Cannot list project directory/.test(error?.message)) throw error;
        const cut = path.lastIndexOf('/');
        const directory = path.slice(0, Math.max(cut, 0)), name = path.slice(cut + 1);
        const items = (await read(directory).catch(() => [])).filter(item => (item.name ?? item) === name);
        if (!items.length) throw new Error(`${path}: no such file or directory in the project`);
        return {directory, items, single: true};
    }
}

/** ls [-l] [-a] [path]. context.manifest() resolves with the manifest for the natures, or null. */
export async function ls(args, {bridge, manifest = async () => null, width = 80}) {
    const {flags, words} = parseArguments('ls', args, 'la');
    if (words.length > 1) throw usageError('ls');
    const path = projectPath(words[0]);
    const all = flags.has('a');
    if (!flags.has('l')) {
        const {items, single} = await listing(bridge, path, directory => listNames(bridge, directory));
        return columns((single ? items : visible(items, all)).map(inline), width);
    }
    const {directory, items, single} = await listing(bridge, path, target => describedEntries(bridge, target));
    const shown = single || all ? items : items.filter(entry => !entry.name.startsWith('.'));
    return longListing(shown, directory, await manifest().catch(() => null));
}

export const catLimit = 1024 * 1024;

async function readText(bridge, path, signal) {
    const chunks = [];
    let size = 0;
    for (;;) {
        signal?.throwIfAborted();
        let chunk;
        try { chunk = await call(bridge, 'read', path, size); }
        catch (error) {
            if (/Cannot read project file/.test(error?.message)) throw new Error(`${path}: no such file in the project`);
            throw error;
        }
        chunks.push(chunk);
        size += chunk.length;
        if (size > catLimit) throw new Error(`${path} is larger than 1 MiB: open it in your editor`);
        if (chunk.length < blockSize) break;
    }
    const bytes = new Uint8Array(await new Blob(chunks).arrayBuffer());
    if (bytes.includes(0)) throw new Error(`${path} is not a text file`);
    try { return new TextDecoder('utf-8', {fatal: true}).decode(bytes); }
    catch { throw new Error(`${path} is not UTF-8 text`); }
}

/** cat <file> [file...]: the text of project files, at most 1 MiB each. */
export async function cat(args, {bridge, signal}) {
    const {words} = parseArguments('cat', args, '');
    if (!words.length) throw usageError('cat');
    let output = '';
    for (const word of words) {
        const path = projectPath(word);
        if (!path) throw new Error(`${word} is a directory`);
        const text = printable(await readText(bridge, path, signal));
        output += text && !text.endsWith('\n') ? text + '\n' : text;
    }
    return output;
}

export const treeLimit = 2000;
const defaultDepth = 3;

/**
 * tree [-a] [-L <depth>] [path]. Hidden entries need -a, and even then .webfront/ is not opened unless the path names
 * it: its package cache would hide the project. The tree stops after treeLimit entries.
 */
export async function tree(args, {bridge, signal}) {
    const {flags, words} = parseArguments('tree', args, 'aL', 'L');
    if (words.length > 1) throw usageError('tree');
    const depth = flags.has('L') ? Number(flags.get('L')) : defaultDepth;
    if (!Number.isInteger(depth) || depth < 1 || depth > 32) throw new Error('tree -L expects a depth from 1 to 32');
    const root = projectPath(words[0]);
    const all = flags.has('a');
    const lines = [root ? inline(root) + '/' : '.'];
    let directories = 0, files = 0, cut = false;
    let rootNames;
    try { rootNames = await listNames(bridge, root); }
    catch (error) {
        if (!/Cannot list project directory/.test(error?.message)) throw error;
        const cutAt = root.lastIndexOf('/');
        const siblings = await listNames(bridge, root.slice(0, Math.max(cutAt, 0))).catch(() => []);
        throw new Error(siblings.includes(root.slice(cutAt + 1)) ? `${root} is a file: cat ${root} prints it` : `${root}: no such directory in the project`);
    }
    // A directory that cannot be listed below the root (removed meanwhile, or unreadable) is shown without content.
    const namesOf = directory => directory === root ? rootNames : listNames(bridge, directory).catch(error => {
        if (/Cannot list project directory/.test(error?.message)) return [];
        throw error;
    });
    const walk = async (directory, prefix, level) => {
        signal?.throwIfAborted();
        const names = visible(await namesOf(directory), all);
        for (const [index, name] of names.entries()) {
            if (lines.length > treeLimit) { cut = true; return; }
            const last = index === names.length - 1;
            const isDirectory = name.endsWith('/');
            const path = join(directory, isDirectory ? name.slice(0, -1) : name);
            const closed = isDirectory && (level === depth || (path === '.webfront' && root !== '.webfront'));
            lines.push(`${prefix}${last ? '└── ' : '├── '}${inline(name)}${closed && level < depth ? '  [managed by wf: tree .webfront]' : ''}`);
            isDirectory ? ++directories : ++files;
            if (isDirectory && !closed) await walk(path, prefix + (last ? '    ' : '│   '), level + 1);
            if (cut) return;
        }
    };
    await walk(root, '', 1);
    if (cut) lines.push(`... more than ${treeLimit} entries: name a subdirectory or lower -L`);
    lines.push('', `${directories} ${directories === 1 ? 'directory' : 'directories'}, ${files} ${files === 1 ? 'file' : 'files'}`);
    return lines.join('\n');
}

const published = [
    ['environment', 'the developer environment, verified against its SHA-512'],
    ['preview', 'the interface built by wf dev and wf preview'],
    ['test', 'the files of a wf test run'],
];

/**
 * mounts: what the developer application answers to the browser, in the order it looks, and what the project
 * commands read. The layers are those of fs::DeveloperFS; the application's own providers are not enumerable.
 */
export async function mounts(args, {bridge, config = {}}) {
    if (args.length) throw usageError('mounts');
    const present = async name => (await listNames(bridge, `.webfront/developer/${name}`).catch(() => [])).length > 0;
    const rows = [
        ['/WebFront.js, /webfront-bootstrap.html, /webfront-bootstrap.mjs', 'embedded in the executable: the bridge and the loader'],
    ];
    for (const [name, purpose] of published)
        rows.push([`/webfront/developer/${name}/`, `.webfront/developer/${name}/ of the project (${await present(name) ? 'published' : 'empty'}): ${purpose}`]);
    rows.push(['/index.html, /favicon.ico, /webfront-dom.mjs, /webfront-fallback.mjs', 'embedded in the executable: the fallback page and its modules'],
        ['other paths', "the application's own providers, if it has some; else 404"]);
    const width = Math.max(...rows.map(([url]) => url.length));
    return ['Served to the browser, the first that answers:',
        ...rows.map(([url, source]) => `  ${url.padEnd(width)}  ${source}`),
        'The sources of the project are never served: wf compiles them into a publication, and wf embed into a C++ provider.',
        '',
        'Read by the commands of this terminal (ls, cat, tree, wf): the project directory given to the application.',
        `  manifest     ${manifestPath}`,
        `  lock         ${lockPath}`,
        '  wf cache     .webfront/  (packages, publications, history)',
        ...(config.environment ? [`  environment  ${config.environment}`] : []),
        ...(config.integrity ? [`  integrity    ${config.integrity}`] : []),
        ...(config.registry ? [`  registry     ${config.registry}${config.registryExplicit ? ' (chosen by the application)' : ''}`] : []),
    ].join('\n');
}

/** The file commands the terminal keeps for itself without providing them (commands.mjs: reservedNames). */
export const withheld = ['cd', 'pwd', 'cp', 'mv', 'rm', 'mkdir', 'rmdir', 'touch', 'edit', 'find', 'grep', 'head', 'tail', 'less', 'stat', 'du', 'df'];
const reading = new Set(['find', 'grep', 'head', 'tail', 'less', 'stat', 'du', 'df']);
/** Why a withheld command does nothing, and what to use instead. */
export function withheldReason(name) {
    if (name === 'cd' || name === 'pwd') return `${name} is not provided: there is no working directory, every path is relative to the project (ls, tree, mounts)`;
    if (reading.has(name)) return `${name} is not provided: this terminal has ls, cat and tree; use your own tools for the rest`;
    return `${name} is not provided: this terminal does not change files; edit the project in your editor (wf dev rebuilds on every save)`;
}
