import {Terminal} from '../../third_party/xterm/xterm.mjs';
import {FitAddon} from '../../third_party/xterm/addon-fit.mjs';
import {LineEditor, tokenize} from '../components/terminal/terminal.mjs';
import {resolvePackages, validateLock, downloadPackage} from './packages.mjs';
import {readProject, publish} from './bootstrap.mjs';
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

export async function install(bridge, specs, {registry, signal, progress = () => {}, fetchImpl = fetch}) {
    if (!specs.length) throw new Error('Usage: wf install <package>[@version] ...');
    const previous = await readLock(bridge, true);
    const requested = new Map(Object.entries(previous.roots).map(([name, root]) => [name, previous.packages[root.id].version]));
    for (const spec of specs) {
        const at = spec.indexOf('@', spec.startsWith('@') ? 1 : 0);
        requested.set(at < 0 ? spec : spec.slice(0, at), at < 0 ? 'latest' : spec.slice(at + 1));
    }
    const {lock, downloads} = await resolvePackages([...requested].map(([name, version]) => `${name}@${version}`), {
        registry, signal, fetchImpl,
        download: async (...args) => { progress(`Resolving ${args[0]}@${args[1]}`); return downloadPackage(...args); },
    });
    for (const [id, files] of downloads) {
        progress(`Storing ${id}`);
        await publish(bridge, `${packageRoot}/${lock.packages[id].key}`, files, true, signal);
    }
    // The lock file is the installation commit point. A failed install may leave unused cache files.
    await publish(bridge, lockPath, new Map([['output', encoder.encode(JSON.stringify(lock, null, 2) + '\n')]]), false, signal);
    return lock;
}

export async function compile(bridge, entry, signal) {
    const lock = await readLock(bridge);
    const packages = {};
    for (const [id, node] of Object.entries(lock.packages)) {
        const files = new Map();
        for (const path of node.files) {
            signal?.throwIfAborted();
            files.set(path, await readProject(bridge, `${packageRoot}/${node.key}/${path}`));
        }
        packages[id] = files;
    }
    const files = await new Promise((resolve, reject) => {
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
            if (data.type === 'read') {
                try { worker.postMessage({type: 'read-result', id: data.id, bytes: await readProject(bridge, data.path)}); }
                catch (error) { worker.postMessage({type: 'read-result', id: data.id, error: error.message}); }
            } else if (data.type === 'result') finish(null, data.files);
            else if (data.type === 'error') finish(new Error(data.error));
        };
        if (signal?.aborted) abort();
        else worker.postMessage({type: 'build', entry, lock, packages});
    });
    files.set('index.html', applicationPage(files));
    const notices = [`WebFront browser build; lock format ${lock.format}`];
    for (const [id, node] of Object.entries(lock.packages).sort()) {
        notices.push(`${id} ${node.integrity}`);
        for (const [path, bytes] of packages[id])
            if (/(^|\/)(licen[cs]e|copying|notice)(\.|$)/i.test(path)) notices.push(`${id}/${path}\n${decoder.decode(bytes)}`);
    }
    return {files, notices: notices.join('\n\n')};
}

export async function execute(args, {bridge, config, preview, signal, write = () => {}}) {
    const [command, ...rest] = args;
    if (command === 'install') {
        const lock = await install(bridge, rest, {registry: config.registry, signal, progress: text => write(text + '\r\n')});
        return `Installed ${Object.keys(lock.packages).length} packages. Lock: ${lockPath}`;
    }
    if (command === 'preview' || command === 'embed') {
        if (!rest[0]) throw new Error(`Usage: wf ${command} <entry.tsx>${command === 'embed' ? ' --output <Provider.hpp>' : ''}`);
        const output = command === 'embed' && rest[1] === '--output' && rest.length === 3 ? rest[2] : null;
        if (command === 'embed' && (!output || !output.endsWith('.hpp'))) throw new Error('wf embed requires --output <Provider.hpp>');
        const result = await compile(bridge, rest[0], signal);
        if (command === 'preview') {
            await publish(bridge, '.webfront/developer/preview', result.files, true, signal);
            preview.hidden = false;
            preview.src = `/webfront/developer/preview/index.html?build=${Date.now()}`;
            return 'Preview ready.';
        }
        const className = output.split('/').pop().slice(0, -4);
        const header = generateProvider(result.files, className, result.notices);
        await publish(bridge, output, new Map([['output', encoder.encode(header)]]), false, signal);
        return `Generated ${output}. Use webfront::fs::${className} and recompile the application.`;
    }
    if (command === 'list') {
        const lock = await readLock(bridge, true);
        return Object.keys(lock.packages).sort().join('\n') || 'No installed packages.';
    }
    if (command === 'help' || !command) return 'wf install <package>[@version] ...\nwf list\nwf preview <entry.tsx>\nwf embed <entry.tsx> --output <Provider.hpp>\nCtrl+C interrupts a browser command. No system shell is exposed.';
    throw new Error(`Unknown wf command: ${command}`);
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
