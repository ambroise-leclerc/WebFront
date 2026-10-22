import {Terminal} from '../../../third_party/xterm/xterm.mjs';
import {FitAddon} from '../../../third_party/xterm/addon-fit.mjs';
import {LineEditor, tokenize} from '../components/terminal/terminal.mjs';
import {validateLock} from './packages.mjs';
import {readProject, publish} from './io.mjs';
import {createStarter} from './starter.mjs';
import {install, initProject} from './installer.mjs';
import {restore, describeRegistry} from './restore.mjs';
import {loadProject, syncNotice, resolveEntry, embedTarget, lockPath, packageRoot, noManifest} from './project.mjs';
import {parseRunOptions, scriptCommand, execTarget, scriptHost, runWorker, runResult, runDeadline, secondsLeft} from './scripts.mjs';
import {generateProvider, applicationPage} from './embed.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
export {lockPath, packageRoot};
export async function readLock(bridge, optional = false) {
    try { return validateLock(JSON.parse(decoder.decode(await readProject(bridge, lockPath)))); }
    catch (error) {
        if (optional && error.message.includes('Cannot read project file:'))
            return {format: 1, roots: {}, packages: {}};
        throw error;
    }
}

const textFile = text => new Map([['output', encoder.encode(text)]]);
export {install};

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

// Serves the worker's project reads and settles the build with its result or error.
async function onWorkerMessage(bridge, worker, finish, data) {
    if (data.type === 'read') worker.postMessage(await answerRead(bridge, data));
    else if (data.type === 'result') finish(null, data.files);
    else if (data.type === 'error') finish(new Error(data.error));
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
        worker.onmessage = ({data}) => onWorkerMessage(bridge, worker, finish, data);
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

const init = (args, {bridge, signal}) => initProject(bridge, args, {signal});

const registryOptions = (config, signal, write) =>
    ({registry: config.registry, registryExplicit: config.registryExplicit === true, signal, progress: text => write(text + '\r\n')});

async function installCommand(args, {bridge, config, signal, write = () => {}}) {
    const {lock, restored, registry} = await install(bridge, args, registryOptions(config, signal, write));
    const count = Object.keys(lock.packages).length;
    const summary = restored || !args.length ? `Installed ${count} packages (${restored} restored).` : `Installed ${count} packages.`;
    return `${summary} ${describeRegistry(registry)}. Lock: ${lockPath}`;
}

async function restoreCommand(args, {bridge, config, signal, write = () => {}}) {
    const result = await restore(bridge, args, registryOptions(config, signal, write));
    return result.registry ? `${result.text}\r\n${describeRegistry(result.registry)}` : result.text;
}

async function preview(args, {bridge, signal, preview: frame}) {
    if (args.length > 1) throw new Error('Usage: wf preview [entry.tsx|name]');
    const project = await loadProject(bridge);
    const entry = resolveEntry(project.manifest, args[0], 'wf preview <entry.tsx>');
    const result = await compile(bridge, entry, signal);
    await publish(bridge, '.webfront/developer/preview', result.files, {signal});
    frame.hidden = false;
    // Replace transient builds without accumulating browser session history.
    frame.contentWindow.location.replace(`/webfront/developer/preview/index.html?build=${Date.now()}`);
    return syncNotice(project) + 'Preview ready.';
}

async function embed(args, {bridge, signal}) {
    const project = await loadProject(bridge);
    const {entry, output, className} = embedTarget(project.manifest, args);
    const result = await compile(bridge, entry, signal);
    await publish(bridge, output, textFile(generateProvider(result.files, className, result.notices)), {directory: false, signal});
    return `${syncNotice(project)}Generated ${output}. Use webfront::fs::${className} and recompile the application.`;
}

// The compiler reads the package files it resolves, on demand, instead of the whole locked graph.
async function compileScript(bridge, lock, entry, signal) {
    const files = await buildInWorker(bridge, {type: 'script', entry, lock}, signal);
    return decoder.decode(files.get('script.js'));
}

const scriptWorker = () => new Worker(new URL('./script-worker.mjs', import.meta.url), {type: 'module'});

async function runModule({bridge, signal, write = () => {}, createWorker = scriptWorker}, project, {label, entry, args, deadline}) {
    const lock = project.lock ?? {format: 1, roots: {}, packages: {}};
    const permissions = project.manifest.permissions ?? {};
    const code = await compileScript(bridge, lock, entry, signal);
    const host = scriptHost({bridge, permissions, lock});
    const outcome = await runWorker({createWorker, host, signal, timeout: secondsLeft(deadline), write,
        message: {code, args, permissions, base: location.href}});
    // A script that ended by itself keeps the writes it did not await; a stopped one does not.
    await (outcome.reason === 'exit' ? host.finish() : host.stop());
    return runResult(label, outcome, host.publications);
}

async function loadManifest(bridge) {
    const project = await loadProject(bridge);
    if (!project.manifest) throw new Error(noManifest);
    return project;
}

// wf run [--timeout <seconds>] <script> [args...]
async function run(args, context) {
    const {timeout, target, args: extra} = parseRunOptions(args, 'wf run [--timeout <seconds>] <script> [args...]');
    const project = await loadManifest(context.bridge);
    const depth = context.depth ?? 0;
    // The limit covers the scripts this one runs through wf commands, whatever their own --timeout.
    const deadline = runDeadline(timeout, context.deadline);
    const command = scriptCommand(project.manifest, target, extra, tokenize, depth);
    if (command.kind === 'wf') return execute(command.argv, {...context, depth: depth + 1, deadline});
    return syncNotice(project) + await runModule(context, project, {label: `Script ${target}`, entry: command.entry, args: command.args, deadline});
}

// wf exec [--timeout <seconds>] <package>[:<entry>] [args...]
async function exec(args, context) {
    const {timeout, target, args: extra} = parseRunOptions(args, 'wf exec [--timeout <seconds>] <package>[:<entry>] [args...]');
    const project = await loadManifest(context.bridge);
    const {id, name, entry} = execTarget(project.lock, target);
    return syncNotice(project) + await runModule(context, project, {label: `${id}:${name}`, entry, args: extra, deadline: runDeadline(timeout, context.deadline)});
}

async function list(_args, {bridge}) {
    const project = await loadProject(bridge);
    const names = Object.keys(project.lock?.packages ?? {}).sort().join('\n');
    return syncNotice(project) + (names || 'No installed packages.');
}

const help = () => 'wf init [name]\nwf create <starter> [directory]\nwf install [--update] [<package>[@range] ...]\nwf restore [--offline]\nwf run [--timeout <seconds>] <script> [args...]\nwf exec [--timeout <seconds>] <package>[:<entry>] [args...]\nwf list\nwf preview [entry.tsx|name]\nwf embed <provider> | <entry.tsx> --output <Provider.hpp>\nCtrl+C interrupts a browser command. No system shell is exposed.';
const commands = {init, create, install: installCommand, restore: restoreCommand, run, exec, preview, embed, list, help};

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
