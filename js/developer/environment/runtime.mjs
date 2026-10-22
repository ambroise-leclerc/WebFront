import {Terminal} from '../../../third_party/xterm/xterm.mjs';
import {FitAddon} from '../../../third_party/xterm/addon-fit.mjs';
import {LineEditor, tokenize, unicodeProvider, clearScreen} from './terminal.mjs';
import {discoverCommands, runCommand} from './commands.mjs';
export {discoverCommands};
import {validateLock} from './packages.mjs';
import {call, readProject, publish} from './io.mjs';
import {isUnsafe} from './paths.mjs';
import {createStarter} from './starter.mjs';
import {install, initProject} from './installer.mjs';
import {restore, describeRegistry} from './restore.mjs';
import {loadProject, syncNotice, resolveEntry, embedTarget, lockPath, packageRoot, noManifest} from './project.mjs';
import {parseRunOptions, scriptCommand, execTarget, scriptHost, runWorker, runResult, runDeadline, secondsLeft} from './scripts.mjs';
import {generateProvider, applicationPage} from './embed.mjs';
import {typescriptPackage, parseCheckOptions, checkHost} from './typecheck.mjs';

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

const checkWorker = () => new Worker(new URL('./typecheck-worker.mjs', import.meta.url), {type: 'module'});

// wf check [--timeout <seconds>]: a TypeScript type check with the locked typescript, in a Worker that writes nothing.
// snapshotBounds lowers the snapshot's bounds for the integration test.
async function check(args, {bridge, signal, write = () => {}, createWorker = checkWorker, deadline, snapshotBounds}) {
    const {timeout} = parseCheckOptions(args);
    const project = await loadManifest(bridge);
    const [typescript] = typescriptPackage(project.lock);
    const host = checkHost(bridge);
    const outcome = await runWorker({createWorker, host, signal, timeout: secondsLeft(runDeadline(timeout, deadline)), write,
        message: {lock: project.lock, typescript, entries: Object.values(project.manifest.entries ?? {}), bounds: snapshotBounds}});
    await host.stop();
    return syncNotice(project) + runResult('Type check', outcome, host.publications);
}

async function list(_args, {bridge}) {
    const project = await loadProject(bridge);
    const names = Object.keys(project.lock?.packages ?? {}).sort().join('\n');
    return syncNotice(project) + (names || 'No installed packages.');
}

const help = () => 'wf init [name]\nwf create <starter> [directory]\nwf install [--update] [<package>[@range] ...]\nwf restore [--offline]\nwf run [--timeout <seconds>] <script> [args...]\nwf exec [--timeout <seconds>] <package>[:<entry>] [args...]\nwf check [--timeout <seconds>]\nwf list\nwf preview [entry.tsx|name]\nwf embed <provider> | <entry.tsx> --output <Provider.hpp>\nCtrl+C interrupts a command. No system shell is exposed.';
const commands = {init, create, install: installCommand, restore: restoreCommand, run, exec, check, preview, embed, list, help};

export async function execute(args, context) {
    const [command = 'help', ...rest] = args;
    if (!Object.hasOwn(commands, command)) throw new Error(`Unknown wf command: ${command}`);
    return commands[command](rest, context);
}

// --- Terminal ----------------------------------------------------------------------------------------------------

export const historyPath = '.webfront/developer/history.json';
const toTerminalText = text => String(text).replace(/(?<!\r)\n/g, '\r\n');

async function readHistory(bridge) {
    try {
        const lines = JSON.parse(decoder.decode(await readProject(bridge, historyPath)));
        return Array.isArray(lines) ? lines.filter(line => typeof line === 'string' && !/[\r\n]/.test(line)) : [];
    } catch { return []; }
}

// The history is written after each command, one write at a time; a failed write keeps the terminal usable.
let historyWrites = Promise.resolve();
function saveHistory(bridge, lines) {
    historyWrites = historyWrites.then(() => publish(bridge, historyPath, textFile(JSON.stringify(lines)), {directory: false})).catch(() => {});
    return historyWrites;
}
/** Resolves once the history written so far is in the project. */
export const historySaved = () => historyWrites;

// Commands of the terminal itself; the application's C++ commands cannot take their names.
const builtins = {
    help: (_args, {cppCommands = new Map()}) => {
        const width = Math.max(7, ...[...cppCommands.keys()].map(name => name.length));
        const line = (name, text) => `${name.padEnd(width)}  ${text}`;
        return [line('wf', 'Project commands: wf help lists them'), line('clear', 'Clear the screen and its scrollback'),
            line('history', 'List the commands typed in this project; history -c clears them'),
            line('exit', 'End the session [with an exit code]'), line('help', 'List the commands'),
            ...[...cppCommands].map(([name, text]) => line(name, text)),
            'Ctrl+C interrupts a command. No system shell is exposed.'].join('\n');
    },
    clear: () => clearScreen,
    history: (args, {editor}) => {
        if (args.length === 1 && args[0] === '-c') {
            editor.history.length = 0;
            editor.historyIndex = 0;
            editor.saveHistory([]);
            return '';
        }
        if (args.length) throw new Error('Usage: history [-c]');
        return editor.history.map((line, index) => `${String(index + 1).padStart(5)}  ${line}`).join('\n');
    },
    exit: (args, {editor, onExit = () => globalThis.close()}) => {
        if (args.length > 1 || (args.length && !/^\d{1,3}$/.test(args[0]))) throw new Error('Usage: exit [code]');
        const code = args.length ? Number(args[0]) : editor?.status ?? 0;
        editor?.close();
        onExit(code);
        return 'Session ended.';
    },
};

/**
 * Runs a terminal line: a built-in command, a wf command or one of the application's C++ commands (context.cppCommands,
 * from discoverCommands). Resolves with the output for the terminal, "\r\n"-terminated lines; context.write receives
 * the output a command streams before that.
 */
export async function runLine(line, context) {
    const [command, ...args] = tokenize(line);
    if (command === undefined) return '';
    const write = text => context.write?.(toTerminalText(text));
    if (command === 'wf') return toTerminalText(await execute(args, {...context, write}));
    if (Object.hasOwn(builtins, command)) return toTerminalText(await builtins[command](args, context));
    if (context.cppCommands?.has(command)) return runCommand(context.bridge, command, args, {signal: context.signal, write});
    throw new Error(`Unknown command: ${command}. Type help to list the commands.`);
}

// The entries of the project directory a partial path names, as completions of that path.
async function projectPaths(bridge, word) {
    const directory = word.slice(0, word.lastIndexOf('/') + 1);
    if (directory && isUnsafe(directory.slice(0, -1))) return [];
    try {
        const names = JSON.parse(decoder.decode(await call(bridge, 'list', directory.slice(0, -1))));
        const hidden = !word.slice(directory.length).startsWith('.');
        return names.filter(name => !(hidden && name.startsWith('.'))).map(name => directory + name);
    } catch { return []; }
}

// The names that the manifest declares for the first argument of a wf command.
async function manifestNames(bridge, command) {
    const section = {run: 'scripts', preview: 'entries', embed: 'providers'}[command];
    if (!section) return [];
    try { return Object.keys((await loadProject(bridge)).manifest?.[section] ?? {}); }
    catch { return []; }
}

/**
 * Completes the word at the end of a line: command names first, then wf subcommands, the manifest's scripts after
 * wf run, its entries and providers after wf preview and wf embed, and project paths. Resolves with {word, candidates}.
 */
export async function complete(line, {bridge, cppCommands = new Map()}) {
    const start = Math.max(line.lastIndexOf(' '), line.lastIndexOf('\t')) + 1;
    const word = line.slice(start);
    const matching = names => ({word, candidates: [...new Set(names)].filter(name => name.startsWith(word)).sort()});
    let before;
    try { before = tokenize(line.slice(0, start)); }
    catch { return matching([]); }
    if (/['"\\]/.test(word)) return matching([]);
    if (!before.length) return matching(['wf', ...Object.keys(builtins), ...cppCommands.keys()]);
    if (before[0] === 'wf' && before.length === 1) return matching(Object.keys(commands));
    if (before[0] === 'wf' && before.length === 2 && before[1] === 'run') return matching(await manifestNames(bridge, 'run'));
    if (before[0] === 'wf' && before.length === 2) return matching([...await manifestNames(bridge, before[1]), ...await projectPaths(bridge, word)]);
    return matching(await projectPaths(bridge, word));
}

/**
 * Mounts the terminal in `element`. Each page has its own session: its history comes from the project, its C++
 * commands run in this browser's session. `onExit(code)` ends the session after exit (by default, closes the window).
 */
export async function mount({bridge, config, terminal: element, preview, onExit}) {
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = new URL('./xterm.css', import.meta.url).href;
    await new Promise((resolve, reject) => {
        link.onload = resolve;
        link.onerror = () => reject(new Error('Cannot load terminal styles'));
        document.head.append(link);
    });
    const terminal = new Terminal({cursorBlink: true, allowProposedApi: true, theme: {background: '#141821'}});
    terminal.unicode.register(unicodeProvider);
    terminal.unicode.activeVersion = unicodeProvider.version;
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    terminal.open(element);
    fit.fit();
    const write = text => terminal.write(text);
    write('WebFront browser-powered developer environment. Type help.\r\n');
    const cppCommands = await discoverCommands(bridge).catch(error => {
        write(`\x1b[31mThe application's commands are unavailable: ${error?.message ?? error}\x1b[0m\r\n`);
        return new Map();
    });
    const context = {bridge, config, preview, cppCommands, onExit};
    const editor = new LineEditor({write, columns: () => terminal.cols, prompt: '> ', history: await readHistory(bridge),
        saveHistory: lines => saveHistory(bridge, lines),
        run: (line, signal) => runLine(line, {...context, editor, signal, write: text => editor.write(text)}),
        complete: line => complete(line, context)});
    terminal.onResize(() => editor.resized());
    new ResizeObserver(() => fit.fit()).observe(element);
    terminal.onData(data => editor.handle(data));
    editor.showPrompt();
    terminal.focus();
    return {terminal, editor};
}
