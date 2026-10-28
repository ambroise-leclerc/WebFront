import {Terminal} from '@xterm/xterm';
import {FitAddon} from '@xterm/addon-fit';
import {LineEditor, tokenize, wordAtEnd, unicodeProvider, clearScreen, historyLimit} from './terminal.mjs';
import {manifestPath, setManifestDirectory} from './manifest.mjs';
import {discoverCommands, runCommand, reservedNames} from './commands.mjs';
export {discoverCommands, reservedNames};
import {validateLock} from './packages.mjs';
import {call, readProject, publish, writeChunks} from './io.mjs';
import {isUnsafe} from './paths.mjs';
import {createStarter} from './starter.mjs';
import {install, initProject} from './installer.mjs';
import {restore, describeRegistry} from './restore.mjs';
import {loadProject, syncNotice, resolveEntry, embedTarget, lockPath, packageRoot, noManifest} from './project.mjs';
import {parseRunOptions, scriptCommand, execTarget, scriptHost, runWorker, runResult, runDeadline, secondsLeft} from './scripts.mjs';
import {build, dev} from './development.mjs';
import {parseTestOptions, specFiles, jasmineCore, runnerFiles, runInFrame, reportResult, fitResult, stoppedRun, abortable, testDirectory} from './testing.mjs';
import {generateProvider, applicationPage} from './embed.mjs';
import {typescriptPackage, parseCheckOptions, checkHost} from './typecheck.mjs';
import {connectAutomation as connectAgent} from './automation.mjs';

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
    else if (data.type === 'error')
        finish(Object.assign(new Error(data.error), Array.isArray(data.diagnostics) ? {diagnostics: data.diagnostics} : {}));
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
    frame.dispatchEvent(new Event('webfront-preview-navigating'));
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
async function check(args, {bridge, signal, write = () => {}, createWorker = checkWorker, deadline, snapshotBounds, onDiagnostics}) {
    const {timeout} = parseCheckOptions(args);
    const project = await loadManifest(bridge);
    const [typescript] = typescriptPackage(project.lock);
    const host = checkHost(bridge);
    const outcome = await runWorker({createWorker, host, signal, timeout: secondsLeft(runDeadline(timeout, deadline)), write,
        message: {lock: project.lock, typescript, entries: Object.values(project.manifest.entries ?? {}), bounds: snapshotBounds}});
    await host.stop();
    onDiagnostics?.(outcome.diagnostics ?? []);
    return syncNotice(project) + runResult('Type check', outcome, host.publications);
}

// wf test [--timeout <seconds>] [pattern]: the specs of "test.specs" under the Jasmine of the lock, in a frame of this page.
// context.onReport receives the structured report, also when the run fails. --timeout, never later than the deadline
// of the wf script that runs this command, covers the whole command: no step waits for the bridge past it, and Ctrl+C
// ends every step as well.
async function test(args, {bridge, signal, write = () => {}, createFrame, onReport, deadline: inherited}) {
    const {timeout, pattern} = parseTestOptions(args);
    const deadline = runDeadline(timeout, inherited);
    const limit = new AbortController();
    // Forward Ctrl+C into the deadline controller: older supported WebKit lacks AbortSignal.any.
    const interrupt = () => limit.abort(signal.reason);
    const preparing = limit.signal;
    const step = promise => abortable(promise, preparing);
    const runId = crypto.randomUUID();
    const directory = `${testDirectory}/${runId}`;
    let jasmineVersion = '';
    let reported = false;
    const finished = report => { reported = true; report.jasmine = jasmineVersion; onReport?.(report); return report; };
    let timer;
    try {
        timer = setTimeout(() => limit.abort(), secondsLeft(deadline) * 1000);
        signal?.addEventListener('abort', interrupt, {once: true});
        if (signal?.aborted) interrupt();
        const project = await step(loadManifest(bridge));
        const specs = specFiles(project.manifest);
        const lock = project.lock ?? {format: 1, roots: {}, packages: {}};
        const jasmine = jasmineCore(lock);
        jasmineVersion = jasmine.node.version;
        const files = await step(buildInWorker(bridge, {type: 'tests', specs, lock}, preparing));
        const jasmineBytes = await step(readProject(bridge, `${packageRoot}/${jasmine.node.key}/${jasmine.path}`));
        await step(publish(bridge, directory, runnerFiles(files, jasmineBytes), {signal: preparing}));
        clearTimeout(timer);
        signal?.removeEventListener('abort', interrupt);
        write(syncNotice(project));
        const report = finished(await runInFrame({pattern, timeout: Math.max(0.001, secondsLeft(deadline)), runId, signal, write, createFrame}));
        write('\n');
        return reportResult(report);
    } catch (error) {
        // An abort of the preparation ends the command like it ends a run: Ctrl+C with 130, the limit with 124, and a report.
        if (!error.report) {
            if (signal?.aborted) error = stoppedRun('interrupted', pattern);
            else if (limit.signal.aborted) error = stoppedRun('timeout', pattern);
        }
        if (error.report && !reported) finished(error.report);
        throw error;
    } finally {
        clearTimeout(timer);
        signal?.removeEventListener('abort', interrupt);
        // The files of the run are not needed any more: an empty directory replaces them, for at most two seconds.
        let cleanupTimer;
        try {
            await Promise.race([publish(bridge, directory, new Map()).catch(() => {}),
                new Promise(resolve => { cleanupTimer = setTimeout(resolve, 2000); })]);
        } finally { clearTimeout(cleanupTimer); }
    }
}

async function list(_args, {bridge}) {
    const project = await loadProject(bridge);
    const names = Object.keys(project.lock?.packages ?? {}).sort().join('\n');
    return syncNotice(project) + (names || 'No installed packages.');
}

const help = () => 'wf init [name]\nwf create <starter> [directory]\nwf install [--update] [<package>[@range] ...]\nwf restore [--offline]\nwf run [--timeout <seconds>] <script> [args...]\nwf exec [--timeout <seconds>] <package>[:<entry>] [args...]\nwf check [--timeout <seconds>]\nwf test [--timeout <seconds>] [pattern]\nwf list\nwf build [entry.tsx|name]\nwf dev [entry.tsx|name]\nwf preview [entry.tsx|name]\nwf embed <provider> | <entry.tsx> --output <Provider.hpp>\nCtrl+C interrupts a command. No system shell is exposed.';
const commands = {init, create, install: installCommand, restore: restoreCommand, run, exec, check, test, build, dev, preview, embed, list, help};

let executionToken = null, automationActive = false, cppActive = 0;
export async function exclusive(context, action) {
    const nested = executionToken !== null && context.executionToken === executionToken;
    if (context.automation && cppActive) throw new Error('Developer environment is busy with an application command');
    if (executionToken !== null && !nested) throw new Error('Developer environment is busy; wait or stop the active operation');
    if (!nested) { executionToken = Symbol('developer operation'); automationActive = context.automation === true; }
    try { return await action({...context, executionToken}); }
    finally { if (!nested) {executionToken = null; automationActive=false;} }
}

export async function execute(args, context) {
    return exclusive(context, async owned => {
        const [command = 'help', ...rest] = args;
        if (!Object.hasOwn(commands, command)) throw new Error(`Unknown wf command: ${command}`);
        return commands[command](rest, owned);
    });
}

export const connectAutomation = context => connectAgent({...context,run:runNonInteractive,exclusive});

/**
 * Runs one wf command without a terminal, for a page that the application drives: the command's output goes to
 * `write`, and the result {exitCode, output, report} is sent to C++ as JSON through `reporter` (for example
 * json => bridge.cppFunction('app.report')(json)) before `onExit(exitCode)` ends the session (by default, closes the
 * window). `report` is the structured report of wf test, null for other commands; the result is cut to fit a bridge string (see fitResult). A command that fails, or a result
 * that cannot be sent, ends with a non-zero code; it never stays open. Resolves with the result.
 */
export async function runNonInteractive(argv, {reporter, onExit = code => globalThis.close(code), write = () => {}, ...context}) {
    let exitCode = 0, output = '', report = null, truncated = false, diagnostics = [];
    const keepDiagnostics = values => {
        diagnostics = (values ?? []).slice(0,10).map(item => ({text:String(item.text ?? '').slice(0,256),
            ...(item.location ? {file:String(item.location.file ?? '').replace(/^project:/,'').slice(0,128),line:item.location.line,column:item.location.column+1} : {}),
            ...(!item.location && item.file ? {file:String(item.file).slice(0,128),line:item.line,column:item.column} : {}),
            ...(item.code === undefined ? {} : {code:item.code})}));
    };
    const append = text => {
        text = String(text ?? '');
        const room = 16000-output.length;
        truncated ||= text.length > room;
        output += text.slice(0,room);
    };
    const collect = text => { append(text); write(text); };
    try {
        setManifestDirectory(context.config?.manifestDirectory);
        // Not `output += await ...`: the left side is read before the command runs, which would drop what it wrote.
        const summary = await execute(argv, {...context, write: collect, onReport: value => { report = value; }, onDiagnostics:keepDiagnostics});
        append(summary);
    } catch (error) {
        exitCode = error?.exitCode ?? 1;
        if (error?.diagnostics) keepDiagnostics(error.diagnostics);
        append(error?.message ?? String(error));
    }
    const result = fitResult({exitCode, output, report, ...(truncated ? {truncated:true} : {}), ...(diagnostics.length ? {diagnostics} : {})});
    try { await reporter?.(JSON.stringify(fitResult(result))); }
    catch (error) {
        result.exitCode = exitCode || 1;
        result.output += `\nThe result could not be sent to the application: ${error?.message ?? error}`;
    }
    onExit(result.exitCode);
    return result;
}

// --- Terminal ----------------------------------------------------------------------------------------------------

export const historyPath = '.webfront/developer/history.json';
// Converts the "\n" line endings of a stream of output pieces to "\r\n", also when a "\r\n" is split between two pieces.
function terminalText() {
    let carriage = false;
    return text => {
        let converted = '';
        for (const char of String(text)) {
            if (char === '\n' && !carriage) converted += '\r';
            converted += char;
            carriage = char === '\r';
        }
        return converted;
    };
}

// The history's lines; none when the project has no history yet. A history that cannot be read or is not a JSON array
// of lines throws: it is shown as empty, but never replaced.
async function readHistory(bridge) {
    let bytes;
    try { bytes = await readProject(bridge, historyPath); }
    catch (error) {
        if (error?.message?.includes('Cannot read project file:')) return [];
        throw error;
    }
    let lines;
    try { lines = JSON.parse(decoder.decode(bytes)); } catch { lines = null; }
    if (!Array.isArray(lines)) throw new Error(`${historyPath} is not a JSON array of lines; it is left unchanged`);
    return lines.filter(line => typeof line === 'string' && !/[\r\n]/.test(line));
}

// The history is written after each command, one write at a time; a failed write keeps the terminal usable. Each change
// applies to the file as it is now, and is committed only if the file did not change since it was read: the terminals
// of other pages on the project, or of other applications, keep their lines.
let historyWrites = Promise.resolve();
// Another page writing the history, or having changed it since it was read: the change is tried again, shortly after.
const historyConflict = /changed since it was read|already being written/;
async function changeHistory(bridge, change) {
    for (let attempt = 1; ; ++attempt) {
        let id = null;
        try {
            const token = await call(bridge, 'state', historyPath);
            const lines = change(await readHistory(bridge)).slice(-historyLimit);
            id = await call(bridge, 'begin', historyPath, false);
            await writeChunks(bridge, {id, path: 'output', bytes: encoder.encode(JSON.stringify(lines))});
            await call(bridge, 'commitGroup', id, historyPath, token);
            return;
        } catch (error) {
            if (id !== null) await call(bridge, 'abort', id).catch(() => {});
            if (attempt === 20 || !historyConflict.test(error?.message)) throw error;
            await new Promise(resolve => setTimeout(resolve, 10 + Math.random() * 40));
        }
    }
}
function writeHistory(bridge, change) {
    historyWrites = historyWrites.then(() => changeHistory(bridge, change)).catch(() => {});
    return historyWrites;
}
const rememberLine = (bridge, line) => writeHistory(bridge, lines => lines.at(-1) === line ? lines : [...lines, line]);
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
    history: async (args, {editor, bridge}) => {
        if (args.length === 1 && args[0] === '-c') {
            editor.history.length = 0;
            editor.historyIndex = 0;
            await writeHistory(bridge, () => []);
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
        // A window that a script did not open stays open: the terminal then waits for a new session.
        return 'Session ended. Press Enter to start a new session.';
    },
};

/** The commands the terminal handles before the application's: wf and the built-ins, which must be reservedNames. */
export const builtinNames = ['wf', ...Object.keys(builtins)];

/**
 * Runs a terminal line: a built-in command, a wf command or one of the application's C++ commands (context.cppCommands,
 * from discoverCommands). Resolves with the output for the terminal, "\r\n"-terminated lines; context.write receives
 * the output a command streams before that.
 */
export async function runLine(line, context) {
    const [command, ...args] = tokenize(line);
    if (command === undefined) return '';
    const toTerminalText = terminalText();  // one per command: its streamed pieces, then its result
    const write = text => context.write?.(toTerminalText(text));
    if (command === 'wf') return toTerminalText(await execute(args, {...context, write}));
    if (Object.hasOwn(builtins, command)) return toTerminalText(await builtins[command](args, context));
    if (context.cppCommands?.has(command)) {
        if (automationActive) throw new Error('Developer environment is busy with an automation operation');
        ++cppActive;
        try { return await runCommand(context.bridge, command, args, {signal: context.signal, write}); }
        finally {--cppActive;}
    }
    throw new Error(`Unknown command: ${command}. Type help to list the commands.`);
}

// The entries of the project directory a partial path names, as completions of that path.
async function projectPaths(bridge, word) {
    if (word.startsWith('/')) return [];  // project paths are relative
    const directory = word.slice(0, word.lastIndexOf('/') + 1);
    if (directory && isUnsafe(directory.slice(0, -1))) return [];
    try {
        const names = JSON.parse(decoder.decode(await call(bridge, 'list', directory.slice(0, -1))));
        const hidden = !word.slice(directory.length).startsWith('.');
        return names.filter(name => !(hidden && name.startsWith('.'))).map(name => directory + name);
    } catch { return []; }
}

// The manifest as last read, kept while the project's state token for it does not change.
let manifestCache = {token: null, manifest: null};
async function cachedManifest(bridge) {
    const token = await call(bridge, 'state', manifestPath);
    if (token !== manifestCache.token) {
        let manifest = null;
        try { manifest = JSON.parse(decoder.decode(await readProject(bridge, manifestPath))); }
        catch { /* no manifest, or one that wf commands report */ }
        manifestCache = {token, manifest};
    }
    return manifestCache.manifest;
}

// The names that the manifest declares for the first argument of a wf command.
async function manifestNames(bridge, command) {
    const section = {run: 'scripts', build: 'entries', dev: 'entries', preview: 'entries', embed: 'providers'}[command];
    if (!section) return [];
    try { return Object.keys((await cachedManifest(bridge))?.[section] ?? {}); }
    catch { return []; }
}

/**
 * Completes the word at the end of a line, escaped or quoted: command names first, then wf subcommands, the manifest's
 * scripts after wf run, its entries and providers after wf preview and wf embed, and project paths. Resolves with
 * {word, candidates}, the word's value and the full words that can replace it.
 */
export async function complete(line, {bridge, cppCommands = new Map()}) {
    const typed = wordAtEnd(line);
    const word = typed?.value ?? '';
    const matching = names => ({word, candidates: [...new Set(names)].filter(name => name.startsWith(word)).sort()});
    if (!typed) return matching([]);
    let before;
    try { before = tokenize(line.slice(0, typed.start)); }
    catch { return matching([]); }
    if (!before.length) return matching(['wf', ...Object.keys(builtins), ...cppCommands.keys()]);
    if (before[0] === 'wf' && before.length === 1) return matching(Object.keys(commands));
    if (before[0] === 'wf' && before.length === 2 && before[1] === 'run') return matching(await manifestNames(bridge, 'run'));
    if (before[0] === 'wf' && before.length === 2) return matching((await Promise.all([manifestNames(bridge, before[1]), projectPaths(bridge, word)])).flat());
    return matching(await projectPaths(bridge, word));
}

/**
 * Mounts the terminal in `element`. Each page has its own session: its history comes from the project, its C++
 * commands run in this browser's session. `onExit(code)` ends the session after exit (by default, closes the window).
 * `fitToElement: false` keeps the terminal's size as set by terminal.resize() instead of following the element's.
 */
export async function mount({bridge, config, terminal: element, preview, onExit, fitToElement = true}) {
    setManifestDirectory(config?.manifestDirectory);
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
    // Keys typed while the commands and the history load are applied once the editor is ready.
    const early = [];
    const earlyInput = terminal.onData(data => early.push(data));
    const [cppCommands, history] = await Promise.all([
        discoverCommands(bridge).catch(error => {
            write(`\x1b[31mThe application's commands are unavailable: ${error?.message ?? error}\x1b[0m\r\n`);
            return new Map();
        }),
        readHistory(bridge).catch(() => [])]);
    earlyInput.dispose();
    const context = {bridge, config, preview, cppCommands, onExit};
    const editor = new LineEditor({write, columns: () => terminal.cols, rows: () => terminal.rows, prompt: '> ', history,
        remember: line => rememberLine(bridge, line),
        run: (line, signal) => runLine(line, {...context, editor, signal, write: text => editor.write(text)}),
        complete: line => complete(line, context)});
    terminal.onResize(() => editor.resized());
    if (fitToElement) new ResizeObserver(() => fit.fit()).observe(element);
    terminal.onData(data => editor.handle(data));
    editor.showPrompt();
    for (const data of early) editor.handle(data);
    terminal.focus();
    return {terminal, editor};
}
