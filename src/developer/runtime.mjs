import {Terminal} from '../../third_party/xterm/xterm.mjs';
import {FitAddon} from '../../third_party/xterm/addon-fit.mjs';
import {LineEditor, tokenize} from '../components/terminal/terminal.mjs';
import {resolvePackages, validateLock, downloadPackage} from './packages.mjs';
import {readProject, publish} from './bootstrap.mjs';
import {createStarter} from './starter.mjs';
import {generateProvider, applicationPage} from './embed.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const lockPath = 'webfront-packages.lock.json';
const packageRoot = '.webfront/developer/packages';
export async function readLock(bridge, optional = false) {
    try { return validateLock(JSON.parse(decoder.decode(await readProject(bridge, lockPath)))); }
    catch (error) {
        if (optional && error.message.includes('Cannot read project file:'))
            return {format: 1, roots: {}, packages: {}};
        throw error;
    }
}

const splitSpec = spec => {
    const at = spec.indexOf('@', spec.startsWith('@') ? 1 : 0);
    return at < 0 ? [spec, 'latest'] : [spec.slice(0, at), spec.slice(at + 1)];
};

// The installed roots keep their exact versions unless a requested spec replaces them.
function requestedRoots(previous, specs) {
    const requested = new Map(Object.entries(previous.roots).map(([name, root]) => [name, previous.packages[root.id].version]));
    for (const spec of specs) requested.set(...splitSpec(spec));
    return [...requested].map(([name, version]) => `${name}@${version}`);
}

const textFile = text => new Map([['output', encoder.encode(text)]]);

export async function install(bridge, specs, {registry, signal, progress = () => {}, fetchImpl = fetch}) {
    if (!specs.length) throw new Error('Usage: wf install <package>[@version] ...');
    const previous = await readLock(bridge, true);
    const {lock, downloads} = await resolvePackages(requestedRoots(previous, specs), {
        registry, signal, fetchImpl, previous,
        download: async (...args) => { progress(`Resolving ${args[0]}@${args[1]}`); return downloadPackage(...args); },
    });
    for (const [id, files] of downloads) {
        progress(`Storing ${id}`);
        await publish(bridge, `${packageRoot}/${lock.packages[id].key}`, files, {signal});
    }
    // The lock file is the installation commit point. A failed install may leave unused cache files.
    await publish(bridge, lockPath, textFile(JSON.stringify(lock, null, 2) + '\n'), {directory: false, signal});
    return lock;
}

async function readPackages(bridge, lock, signal) {
    const packages = {};
    for (const [id, node] of Object.entries(lock.packages)) {
        packages[id] = new Map();
        for (const path of node.files) {
            signal?.throwIfAborted();
            packages[id].set(path, await readProject(bridge, `${packageRoot}/${node.key}/${path}`));
        }
    }
    return packages;
}

function buildInWorker(bridge, message, signal) {
    return new Promise((resolve, reject) => {
        const worker = new Worker(new URL('./compiler.mjs', import.meta.url), {type: 'module'});
        const finish = (error, files) => {
            worker.terminate();
            clearTimeout(timeout);
            signal?.removeEventListener('abort', abort);
            error ? reject(error) : resolve(files);
        };
        const abort = () => finish(signal.reason ?? new Error('Interrupted'));
        const timeout = setTimeout(() => finish(new Error('Browser build timed out')), 120000);
        signal?.addEventListener('abort', abort, {once: true});
        worker.onerror = event => finish(new Error(event.message || 'Compiler worker failed'));
        worker.onmessage = async ({data}) => {
            if (data.type === 'read') worker.postMessage(await answerRead(bridge, data));
            else if (data.type === 'result') finish(null, data.files);
            else if (data.type === 'error') finish(new Error(data.error));
        };
        if (signal?.aborted) abort();
        else worker.postMessage(message);
    });
}

async function answerRead(bridge, {id, path}) {
    try { return {type: 'read-result', id, bytes: await readProject(bridge, path)}; }
    catch (error) { return {type: 'read-result', id, error: error.message}; }
}

const licenseFile = /(^|\/)(licen[cs]e|copying|notice)(\.|$)/i;

function dependencyNotices(lock, packages) {
    const notices = [`WebFront browser build; lock format ${lock.format}`];
    for (const [id, node] of Object.entries(lock.packages).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)) {
        notices.push(`${id} ${node.integrity}`);
        for (const [path, bytes] of packages[id])
            if (licenseFile.test(path)) notices.push(`${id}/${path}\n${decoder.decode(bytes)}`);
    }
    return notices.join('\n\n');
}

export async function compile(bridge, entry, signal) {
    const lock = await readLock(bridge);
    const packages = await readPackages(bridge, lock, signal);
    const files = await buildInWorker(bridge, {type: 'build', entry, lock, packages}, signal);
    files.set('index.html', applicationPage(files));
    return {files, notices: dependencyNotices(lock, packages)};
}

async function create([starter, directory, ...extra], {bridge, config, signal}) {
    if (!starter || extra.length) throw new Error('Usage: wf create <starter> [directory]');
    return createStarter(bridge, config, [starter, directory ?? 'ui'], {signal});
}

async function installCommand(specs, {bridge, config, signal, write = () => {}}) {
    const lock = await install(bridge, specs, {registry: config.registry, signal, progress: text => write(text + '\r\n')});
    return `Installed ${Object.keys(lock.packages).length} packages. Lock: ${lockPath}`;
}

async function preview([entry], {bridge, signal, preview: frame}) {
    if (!entry) throw new Error('Usage: wf preview <entry.tsx>');
    const result = await compile(bridge, entry, signal);
    await publish(bridge, '.webfront/developer/preview', result.files, {signal});
    frame.hidden = false;
    // Replace transient builds without accumulating browser session history.
    frame.contentWindow.location.replace(`/webfront/developer/preview/index.html?build=${Date.now()}`);
    return 'Preview ready.';
}

async function embed([entry, flag, output, ...extra], {bridge, signal}) {
    if (!entry) throw new Error('Usage: wf embed <entry.tsx> --output <Provider.hpp>');
    if (flag !== '--output' || extra.length || !output?.endsWith('.hpp')) throw new Error('wf embed requires --output <Provider.hpp>');
    const result = await compile(bridge, entry, signal);
    const className = output.split('/').pop().slice(0, -4);
    await publish(bridge, output, textFile(generateProvider(result.files, className, result.notices)), {directory: false, signal});
    return `Generated ${output}. Use webfront::fs::${className} and recompile the application.`;
}

async function list({bridge}) {
    const lock = await readLock(bridge, true);
    return Object.keys(lock.packages).sort().join('\n') || 'No installed packages.';
}

const help = () => 'wf create <starter> [directory]\nwf install <package>[@version] ...\nwf list\nwf preview <entry.tsx>\nwf embed <entry.tsx> --output <Provider.hpp>\nCtrl+C interrupts a browser command. No system shell is exposed.';
const commands = {create, install: installCommand, preview, embed, list, help};

export async function execute(args, context) {
    const [command = 'help', ...rest] = args;
    if (!Object.hasOwn(commands, command)) throw new Error(`Unknown wf command: ${command}`);
    return commands[command](rest, context);
}

export async function mount({bridge, config, terminal: element, preview}) {
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = new URL('./xterm.css', import.meta.url).href;
    await new Promise((resolve, reject) => {
        link.onload = resolve;
        link.onerror = () => reject(new Error('Cannot load terminal styles'));
        document.head.append(link);
    });
    const terminal = new Terminal({cursorBlink: true, theme: {background: '#141821'}});
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    terminal.open(element);
    fit.fit();
    new ResizeObserver(() => fit.fit()).observe(element);
    const write = text => terminal.write(text);
    const editor = new LineEditor({write, prompt: 'wf> ', complete: async () => 'wf', run: async (line, signal) => {
        const args = tokenize(line);
        if (args.shift() !== 'wf') throw new Error('Use wf help to list commands');
        return execute(args, {bridge, config, preview, signal, write});
    }});
    terminal.onData(data => editor.handle(data));
    write('WebFront browser-powered developer environment\r\n');
    editor.showPrompt();
    terminal.focus();
    return {terminal, editor};
}
