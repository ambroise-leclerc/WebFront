import {withTimers, untilReady} from './timers.mjs';
// Autonomous real-engine test: compiled WASM, npm graph, Life bridge, preview, cache and embedding.
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const assert = (condition, message) => { if (!condition) throw new Error(message); };
const profileNow = performance.now.bind(performance);
let lastPhase;
const progress = async message => {
    const time = profileNow();
    if (lastPhase) await webFront.cppFunction('test.progress')(
        `${time.toFixed(3)}ms end ${lastPhase.name} durationMs=${(time - lastPhase.time).toFixed(3)}`);
    lastPhase = {name: message, time};
    await webFront.cppFunction('test.progress')(`${time.toFixed(3)}ms begin ${message}`);
};
const call = (name, ...args) => webFront.cppFunction(`webfront.developer.${name}`)(...args);
async function readFile(path) {
    const chunks = [];
    for (let offset = 0;;) {
        const chunk = await call('read', path, offset);
        chunks.push(chunk);
        offset += chunk.length;
        if (chunk.length < 256 * 1024) return new Uint8Array(await new Blob(chunks).arrayBuffer());
    }
}
async function writeFile(path, text) {
    const id = await call('begin', path, false);
    await call('write', id, 'output', 0, new TextEncoder().encode(text));
    await call('commit', id);
}
// The terminal of the page: the harness shows there each wf command it runs, as if it had been typed.
let traceTerminal, traceWarned = false;
const echo = text => {
    try { traceTerminal?.write(`\x1b[2m${text}\x1b[0m\r\n`); }
    catch (error) { if (!traceWarned) { traceWarned = true; console.warn(`The command trace is unavailable: ${error?.message ?? error}`); } }  // it must never fail a test
};
// Only the wf subcommand is shown (never an argument, an option value or a path). Only the deliberate waits are named as
// such: many specs also expect a rejection, which the trace cannot tell from a defect.
const outcomeOf = error => !error ? 'done' : error.exitCode === 124 ? 'timeout (expected)' : error.exitCode === 130 ? 'cancelled (expected)' : 'rejected';
const traced = async (line, run) => {
    const started = profileNow();
    echo(`$ ${line}`);
    let error;
    try { return await run(); }
    catch (thrown) { error = thrown; throw thrown; }
    finally { echo(`  ${outcomeOf(error)} in ${((profileNow() - started) / 1000).toFixed(1)}s`); }
};
// Every actual command has a matching end marker, including expected failures.
const execute = async (args, context) => {
    const name = `command ${args.slice(0, 3).join(' ')}`;
    const started = profileNow();
    await webFront.cppFunction('test.progress')(`${started.toFixed(3)}ms begin ${name}`);
    try { return await traced(`wf ${args[0] ?? ''}`, () => runtimeExecute(args, context)); }
    finally { await webFront.cppFunction('test.progress')(`${profileNow().toFixed(3)}ms end ${name} durationMs=${(profileNow() - started).toFixed(3)}`); }
};
let runtimeExecute, runtimeNonInteractive;
const nonInteractive = async (args, context) => {
    const started = profileNow();
    await webFront.cppFunction('test.progress')(`${started.toFixed(3)}ms begin non-interactive ${args.join(' ')}`);
    try { return await traced(`wf ${args[0] ?? ''} (non-interactive)`, () => runtimeNonInteractive(args, context)); }
    finally { await webFront.cppFunction('test.progress')(`${profileNow().toFixed(3)}ms end non-interactive ${args.join(' ')} durationMs=${(profileNow() - started).toFixed(3)}`); }
};
// Ends the --public smoke test, which has no local registry for the checks that follow the generated headers.
class PublicSmokeDone extends Error {}
try {
    await webFront.ready;
    for (let attempt = 0; attempt < 300; ++attempt) {
        const text = document.getElementById('status')?.textContent ?? '';
        if (text.startsWith('Bootstrap failed:')) throw new Error(text);
        if (text.startsWith('Developer environment ready.')) break;
        if (attempt === 299) throw new Error('Bootstrap readiness timed out');
        await delay(100);
    }
    await progress('bootstrap ready');
    const httpResponses = await Promise.all([
        fetch('/fixtures/session.html', {cache: 'no-store'}),
        fetch('/fixtures/session.html', {method: 'POST', cache: 'no-store'}),
    ]);
    assert(httpResponses[0].status === 200 && httpResponses[1].status === 501,
        'Concurrent HTTP responses must retain their own status lines');
    for (const response of httpResponses) {
        assert(response.headers.get('Connection')?.toLowerCase() === 'close',
            'HTTP responses must announce closure while the bridge WebSocket stays connected');
        await response.arrayBuffer();
    }
    const runtime = await import('/webfront/developer/environment/runtime.mjs');
    runtimeExecute = runtime.execute;
    runtimeNonInteractive = runtime.runNonInteractive;
    const config = JSON.parse(await webFront.cppFunction('webfront.developer.config')());
    // The bootstrap's terminal is not reachable: mount the same one again, without a second resize observer or stylesheet,
    // and clear its prompt, to write the trace of the commands in it.
    const terminalElement = document.getElementById('terminal');
    const styleSheets = document.querySelectorAll('link[rel=stylesheet][href$="xterm.css"]').length;
    terminalElement.replaceChildren();
    traceTerminal = (await runtime.mount({bridge: webFront, config, terminal: terminalElement, preview: document.getElementById('preview'), fitToElement: false})).terminal;
    [...document.querySelectorAll('link[rel=stylesheet][href$="xterm.css"]')].slice(styleSheets).forEach(link => link.remove());
    traceTerminal.reset();
    const environment = await (await fetch('/webfront/developer/environment/environment.json')).json();
    const currentDistribution = !config.environment.startsWith('https:');
    if (currentDistribution) assert(JSON.stringify(Object.keys(environment.starters)) === JSON.stringify(['life']), 'Only the Life starter must be shipped');
    config.starters = Object.fromEntries(Object.entries(environment.starters).map(([name, value]) => [name, {url: new URL(value.archive, new URL(config.environment, location.href)).href, integrity: value.integrity}]));
    const context = {bridge: webFront, config, preview: document.getElementById('preview'), signal: new AbortController().signal,
        write: text => console.log(text)};
    if (currentDistribution) assert(await execute(['create', 'pi'], context).then(() => false, error => /Unknown pinned starter/.test(error.message)),
        'The retired PI starter must not be offered');
    await execute(['create', 'life'], context);
    const overwriteRefused = await execute(['create', 'life'], context).then(() => false, () => true);
    assert(overwriteRefused, 'A starter must not replace an existing project');
    const noManifest = await execute(['install', 'react@19.3.1'], context).then(() => false, error => /run wf init/.test(error.message));
    assert(noManifest, 'wf install must ask for a manifest first');
    await execute(['init'], context);
    const initRefused = await execute(['init'], context).then(() => false, () => true);
    assert(initRefused, 'wf init must not replace an existing manifest');
    await execute(['install', 'react@19.3.1', 'react-dom@19.3.1'], context);
    let lock = await runtime.readLock(webFront);
    assert(lock.manifestDigest?.length === 64, 'The lock must record the digest of the manifest dependencies');
    const manifest = JSON.parse(new TextDecoder().decode(await readFile('webfront.project.json')));
    assert(manifest.dependencies.react === '19.3.1' && manifest.dependencies['react-dom'] === '19.3.1', 'The manifest must record the requested ranges');
    // Entries and providers declared in the manifest drive wf preview and wf embed.
    manifest.entries = {main: 'ui/main.tsx'};
    manifest.providers = {LifeUI: {entry: 'main', output: 'generated/LifeUI.hpp'}};
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    assert(!(await execute(['list'], context)).includes('Warning'), 'Editing entries must not desynchronize the lock');
    assert(Object.keys(lock.packages).length === 3, 'React, React DOM and scheduler must be resolved');
    const listed = await execute(['list'], context);
    assert(listed.split('\n').length === 3 && listed.includes('react@19.3.1'), 'wf list must name the installed packages');
    const historyLength = history.length;
    const frame = context.preview;
    // wf build does not navigate; wf dev keeps one WASM context, reloads only committed output and recovers.
    assert((await runtime.complete('wf d', context)).candidates.includes('dev'), 'Completion must list wf dev');
    assert((await runtime.complete('wf build m', context)).candidates.includes('main') &&
        (await runtime.complete('wf dev m', context)).candidates.includes('main'), 'Build/dev completion must list manifest entries');
    const frameBeforeBuild = frame.contentDocument;
    const frameURLBeforeBuild = frame.contentWindow.location.href;
    await progress('build and incremental preview');
    await execute(['build'], context);
    assert(frame.contentDocument === frameBeforeBuild && frame.contentWindow.location.href === frameURLBeforeBuild,
        'wf build must leave the preview alone');
    assert((await readFile('.webfront/developer/build/app.js')).length > 0, 'wf build must publish compiled JS');
    assert(new TextDecoder().decode(await readFile('.webfront/developer/build/index.html')).includes('./app.js'), 'wf build must publish HTML and JS');
    const waitDev = async (predicate, message) => {
        for (let attempt = 0; !predicate(); ++attempt) {
            if (attempt === 300) throw new Error(message);
            await delay(30);
        }
    };
    await writeFile('watch/main.ts', "import {value} from './value'; document.getElementById('root').textContent = value;\n");
    await writeFile('watch/value.ts', "export const value: string = 'watch-one';\n");
    let devOutput = '';
    const devStop = new AbortController();
    await withTimers([750], async clock => {
        const poll = async () => {
            await waitDev(() => clock.pending() > 0, 'Dev polling deadline was not armed');
            clock.fire(750);
        };
        const devResult = execute(['dev', 'watch/main.ts'], {...context, signal: devStop.signal,
            write: text => { devOutput += text; }}).then(() => null, error => error);
        try {
            await waitDev(() => frame.contentDocument?.getElementById('root')?.textContent === 'watch-one', 'Initial dev preview did not mount');
            await writeFile('watch/value.ts', "export const value: string = 'watch-two';\n");
            await poll();
            await waitDev(() => frame.contentDocument?.getElementById('root')?.textContent === 'watch-two', 'Changed dependency was not rebuilt');
            const lastGoodDocument = frame.contentDocument;
            const lastGoodJS = new TextDecoder().decode(await readFile('.webfront/developer/preview/app.js'));
            await writeFile('watch/value.ts', 'export const value = ;\n');
            await poll();
            await waitDev(() => frame.contentDocument?.getElementById('webfront-build-diagnostics'), 'Syntax error was not overlaid');
            assert(frame.contentDocument === lastGoodDocument && frame.contentDocument.getElementById('root').textContent === 'watch-two', 'A syntax error must preserve the running preview');
            assert(new TextDecoder().decode(await readFile('.webfront/developer/preview/app.js')) === lastGoodJS, 'A syntax error must preserve published output');
            assert(/watch\/value.ts:1:/.test(devOutput) && /watch\/value.ts:1:/.test(frame.contentDocument.getElementById('webfront-build-diagnostics').textContent), 'Terminal and overlay must locate the syntax error');
            await writeFile('watch/value.ts', "export const value: string = 'watch-three';\n");
            await poll();
            await waitDev(() => frame.contentDocument?.getElementById('root')?.textContent === 'watch-three', 'Corrected source did not recover');
            assert(!frame.contentDocument.getElementById('webfront-build-diagnostics'), 'Successful build must clear the overlay');
            await writeFile('watch/main.ts', "import {value} from './created'; document.getElementById('root').textContent = value;\n");
            await poll();
            await waitDev(() => frame.contentDocument?.getElementById('webfront-build-diagnostics'), 'Missing import was not reported');
            await writeFile('watch/created.ts', "export const value = 'watch-created';\n");
            await poll();
            await waitDev(() => frame.contentDocument?.getElementById('root')?.textContent === 'watch-created', 'Creating a missing import must recover');
            assert(history.length === historyLength, 'Dev reloads must replace session history');
        } finally { devStop.abort(); }
        assert((await devResult)?.exitCode === 130, 'Ctrl+C must stop dev with exit code 130');
        const afterStop = frame.contentDocument;
        await writeFile('watch/created.ts', "export const value = 'must-not-rebuild';\n");
        assert(clock.pending() === 0, 'Stopped dev must remove its polling timer');
        assert(frame.contentDocument === afterStop && frame.contentDocument.getElementById('root').textContent === 'watch-created', 'Stopped dev must not rebuild or reload');
        assert(JSON.parse(new TextDecoder().decode(await call('list', '.webfront/developer/staging'))).length === 0, 'Stopped dev must leave no staging');
    });
    // Each named entry gets its own relative HTML/assets when building all manifest entries.
    await writeFile('webfront.project.json', JSON.stringify({...manifest, entries: {one: 'watch/value.ts', two: 'watch/created.ts'}}));
    await execute(['build'], context);
    for (const name of ['one', 'two']) assert((await readFile(`.webfront/developer/build/${name}/index.html`)).length > 0, `Missing build entry ${name}`);
    const firstEntryBuild = new TextDecoder().decode(await readFile('.webfront/developer/build/one/app.js'));
    const secondEntryBuild = new TextDecoder().decode(await readFile('.webfront/developer/build/two/app.js'));
    await writeFile('watch/created.ts', 'export const value = ;\n');
    assert(await execute(['build'], context).then(() => false, () => true), 'A failed build must reject');
    assert(new TextDecoder().decode(await readFile('.webfront/developer/build/one/app.js')) === firstEntryBuild &&
        new TextDecoder().decode(await readFile('.webfront/developer/build/two/app.js')) === secondEntryBuild, 'Failure in a later entry must preserve the entire previous build');
    await writeFile('watch/created.ts', "export const value = 'must-not-rebuild';\n");
    await writeFile('webfront.project.json', JSON.stringify(manifest));
    await progress('Life preview and bridge');
    // Exercise the maintained public application rather than the retired PI demo.
    await execute(['preview'], context);
    for (let attempt = 0; !frame.contentDocument?.querySelector('[data-testid="pattern-blinker"]'); ++attempt) {
        if (attempt === 200) throw new Error('Life preview did not mount');
        await delay(50);
    }
    assert(history.length === historyLength, 'Preview rebuilds must not accumulate session history');
    const life = frame.contentDocument;
    const button = name => life.querySelector(`[data-testid="${name}"]`);
    const waitFor = async (predicate, message) => {
        for (let attempt = 0; !predicate(); ++attempt) {
            if (attempt === 100) throw new Error(message);
            await delay(30);
        }
    };
    const packed = new Uint8Array(128);
    for (const i of [330, 331, 332]) packed[i >> 3] |= 1 << (i & 7);
    const next = await webFront.cppFunction('life.step')(packed);
    if (!(next instanceof Uint8Array) || next.length !== 128) throw new Error('Expected 128 packed Life bytes');
    const expected = new Uint8Array(128);
    for (const i of [299, 331, 363]) expected[i >> 3] |= 1 << (i & 7);
    if (!next.every((byte, i) => byte === expected[i])) throw new Error('Incorrect packed Life bit order');
    for (const value of [new Uint8Array(127), new Int8Array(128), '0'.repeat(1024)]) {
        const rejected = await webFront.cppFunction('life.step')(value).then(() => false, () => true);
        if (!rejected) throw new Error('Malformed Life grid accepted');
    }
    button('pattern-blinker').click();
    await waitFor(() => button('population').textContent === '3', 'Blinker population');
    button('step').click();
    await waitFor(() => button('generation').textContent === '0001', 'C++ Life step');
    assert(button('population').textContent === '3', 'Blinker must preserve its population');
    button('play').click();
    await waitFor(() => Number(button('generation').textContent) >= 3, 'Life animation');
    button('play').click();
    // Clear invalidates a possible in-flight generation and stops the timer.
    button('clear').click();
    await delay(400);
    assert(button('generation').textContent === '0000' && button('population').textContent === '0', 'Clear must discard stale replies');
    // Paint across an octet boundary, then let C++ eliminate the two isolated cells.
    button('view').click();
    await waitFor(() => button('view').getAttribute('aria-pressed') === 'true', 'Plan view');
    const canvas = life.querySelector('canvas');
    const paint = x => {
        const rect = canvas.getBoundingClientRect();
        canvas.dispatchEvent(new frame.contentWindow.MouseEvent('click', {bubbles: true,
            clientX: rect.left + (209 + x * 17 + 7) * rect.width / 960,
            clientY: rect.top + (65 + 7) * rect.height / 680}));
    };
    paint(7);
    await waitFor(() => button('population').textContent === '1', 'Plant the last bit of an octet');
    paint(7);
    await waitFor(() => button('population').textContent === '0', 'Toggle a packed cell off');
    paint(7);
    await waitFor(() => button('population').textContent === '1', 'Plant the first cell');
    paint(8);
    await waitFor(() => button('population').textContent === '2', 'Plant across the octet boundary');
    button('step').click();
    await waitFor(() => button('generation').textContent === '0001' && button('population').textContent === '0', 'C++ evolves painted packed cells');
    button('view').click();
    button('pattern-glider').click();
    await waitFor(() => button('population').textContent === '5', 'Glider population');
    for (let i = 1; i <= 4; i++) {
        await waitFor(() => !button('step').disabled, 'Single step readiness');
        button('step').click();
        await waitFor(() => Number(button('generation').textContent) === i, 'Glider generation');
    }
    assert(button('population').textContent === '5', 'C++ glider survives four steps');
    assert(life.querySelector('canvas').width > 0 && !life.querySelector('[role="alert"]').textContent, 'Life canvas rendered without errors');
    await execute(['embed', 'LifeUI'], context);
    const lifeHeader = await webFront.cppFunction('test.header')('LifeUI');
    await execute(['embed', 'ui/main.tsx', '--output', 'generated/LifeUI.hpp'], context);
    assert(await webFront.cppFunction('test.header')('LifeUI') === lifeHeader, 'Life header must be deterministic');
    if (!config.registryExplicit) throw new PublicSmokeDone();
    // The application path above needs React DOM. Cache/command regressions do not: keep a small
    // real graph instead of repeatedly transferring React DOM's server/development files through the bridge.
    manifest.dependencies = {react: '19.3.1', 'webfront-fixture-lib': '1.0.0'};
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    await execute(['install', '--update'], context);
    lock = await runtime.readLock(webFront);
    assert(Object.keys(lock.packages).length === 2 && !lock.roots['react-dom'], 'Command fixtures must not retain the application dependency graph');
    // A dependency added by hand desynchronizes the lock: commands say so and nothing is resolved implicitly.
    manifest.dependencies.scheduler = '*';
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    assert((await execute(['list'], context)).startsWith('Warning'), 'A desynchronized lock must be reported');
    const refused = await execute(['install'], context).then(() => false, error => /wf install --update/.test(error.message));
    assert(refused, 'wf install must not resolve a desynchronized manifest');
    delete manifest.dependencies.scheduler;
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    assert(/Installed 2 packages/.test(await execute(['install'], context)), 'A consistent lock must install its exact graph');
    // wf restore rebuilds an emptied cache from the lock alone, from the application's local registry although the
    // manifest names another one; --offline only checks the cache, without any request.
    const requests = [];
    const pageFetch = globalThis.fetch;
    globalThis.fetch = (input, init) => {
        requests.push(new URL(input instanceof Request ? input.url : input, location.href).href);
        return pageFetch(input, init);
    };
    try {
        const emptied = await call('begin', '.webfront/developer/packages', true);
        await call('commit', emptied);
        const missing = await execute(['restore', '--offline'], context).then(() => '', error => error.message);
        assert(/react@19\.3\.1 \(missing\)/.test(missing) && requests.length === 0, 'An offline check must list missing packages without any request');
        await writeFile('webfront.project.json', JSON.stringify({...manifest, registry: 'https://registry.invalid/'}, null, 2));
        const restored = await execute(['restore'], context);
        assert(/Restored 2 of 2 packages/.test(restored) && restored.includes('Registry: /fixtures/registry/ (application option)'), `wf restore must use and name the application registry: ${restored}`);
        assert(requests.length > 0 && requests.every(url => url.startsWith(`${location.origin}/fixtures/registry/`)), 'wf restore must contact the application registry only');
        requests.length = 0;
        assert(/Cache verified offline: 2 packages/.test(await execute(['restore', '--offline'], context)) && requests.length === 0, 'A complete cache must verify offline');
    } finally {
        globalThis.fetch = pageFetch;
    }
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    const previous = frame.contentWindow.location.href;
    await writeFile('restore/main.js', "import value from 'webfront-fixture-lib'; document.getElementById('root').textContent = value;\n");
    await execute(['preview', 'restore/main.js'], context);
    const restoredMounted = () => frame.contentDocument?.getElementById('root')?.textContent === 'webfront-fixture-lib@1.0.0';
    for (let attempt = 0; frame.contentWindow.location.href === previous || !restoredMounted(); ++attempt) {
        if (attempt === 300) throw new Error('The preview did not build from the restored cache');
        await delay(50);
    }
    const absent = await execute(['install', 'webfront-fixture-absent'], context).then(() => '', error => error.message);
    assert(/HTTP 404/.test(absent), 'A package absent from the registry must reject');
    const failed = await execute(['install', 'INVALID-NPM-NAME'], context).then(() => false, () => true);
    assert(failed, 'Missing packages must reject');
    assert(JSON.stringify(await runtime.readLock(webFront)) === JSON.stringify(lock), 'A failed install must preserve the lock');
    await progress('scripts and cancellation');
    // wf run and wf exec: WebFront scripts in a dedicated Worker, under the manifest permissions.
    const scriptOutput = [];
    const runContext = (signal = new AbortController().signal) => ({...context, signal, write: text => { scriptOutput.push(text); console.log(text); }});
    const printed = async text => {
        for (let attempt = 0; !scriptOutput.some(line => line.includes(text)); ++attempt) {
            if (attempt === 200) throw new Error(`The script did not print ${text}`);
            await delay(25);
        }
    };
    const failure = promise => promise.then(() => null, error => error);
    const scripts = {
        'tools/gen.ts': "export default async function (context: {args: string[]; project: any; console: Console}) {\n    const text: string = context.args.join('+');\n    await context.project.write('generated/script/out.txt', text);\n    context.console.log('wrote', text);\n}\n",
        'tools/denied.mjs': "export default async context => { await context.project.write('ui/main.tsx', 'replaced'); };\n",
        'tools/node.mjs': "import {readFileSync} from 'fs';\nexport default async () => readFileSync;\n",
        'tools/fail.mjs': "export default async context => context.exit(3);\n",
        'tools/coop.mjs': "export default context => new Promise((resolve, reject) => {\n    context.signal.addEventListener('abort', () => reject(context.signal.reason));\n    context.console.log('waiting');\n});\n",
        'tools/loop.mjs': "export default async context => { context.console.log('looping'); for (;;) {} };\n",
        'tools/big.mjs': "export default async context => { context.console.log('writing'); await context.project.write('generated/script/big.bin', new Uint8Array(4 * 1024 * 1024).fill(1)); };\n",
        'tools/unawaited.mjs': "export default async context => { context.project.write('generated/script/unawaited.txt', 'done'); };\n",
        'tools/root.mjs': "export default async context => context.console.log('root', JSON.stringify(await context.project.list()));\n",
        'tools/jsx.tsx': "export default async (context: any) => { const element = <b>x</b>; context.console.log('jsx', element.type); };\n",
    };
    for (const [path, text] of Object.entries(scripts)) await writeFile(path, text);
    const mainBefore = await readFile('ui/main.tsx');
    const scriptManifest = {...manifest, permissions: {files: ['tools'], publish: ['generated/script']},
        scripts: Object.fromEntries([...Object.keys(scripts).map(path => [path.split('/')[1].split('.')[0], path]), ['listing', 'wf list'], ['nested', 'wf run --timeout 120 loop']])};
    await writeFile('webfront.project.json', JSON.stringify(scriptManifest, null, 2));
    assert(/Script gen completed/.test(await execute(['run', 'gen', 'a', 'b'], runContext())), 'A TypeScript script must run');
    assert(new TextDecoder().decode(await readFile('generated/script/out.txt')) === 'a+b' && scriptOutput.includes('wrote a+b\r\n'), 'The script must write an allowed file and stream its console');
    assert(/react@19\.3\.1/.test(await execute(['run', 'listing'], runContext())), 'A script may run a wf command');
    const denied = await failure(execute(['run', 'denied'], runContext()));
    assert(/Permission denied: writing ui\/main\.tsx/.test(denied?.message) && denied.exitCode === 1, `A context operation outside the permissions must be refused: ${denied}`);
    assert(new TextDecoder().decode(await readFile('ui/main.tsx')) === new TextDecoder().decode(mainBefore), 'A refused write must not change the file');
    const node = await failure(execute(['run', 'node'], runContext()));
    assert(/"fs" is a Node\.js API/.test(node?.message), `A Node.js import must be refused while loading: ${node}`);
    const exited = await failure(execute(['run', 'fail'], runContext()));
    assert(exited?.exitCode === 3 && /exited with code 3/.test(exited.message), 'A non-zero exit code must be propagated');
    const cooperative = new AbortController();
    const waiting = failure(execute(['run', 'coop'], runContext(cooperative.signal)));
    await printed('waiting');
    cooperative.abort(new Error('Interrupted'));
    const stopped = await waiting;
    assert(stopped?.exitCode === 130 && !/grace period/.test(stopped.message), `Ctrl+C must stop a cooperative script: ${stopped}`);
    const interrupt = new AbortController();
    const looping = failure(execute(['run', 'loop'], runContext(interrupt.signal)));
    await printed('looping');
    await withTimers([2000], async clock => {
        interrupt.abort(new Error('Interrupted'));
        clock.fire(2000);
        const killed = await looping;
        assert(killed?.exitCode === 130 && /terminated after the 2 s grace period/.test(killed.message), `A busy script must be terminated after Ctrl+C: ${killed}`);
    });
    // Real Worker/bridge preparation gets a generous bound; only the deadline under test is advanced.
    for (const target of ['loop', 'nested']) {
        scriptOutput.length = 0;
        await withTimers([60000, 2000], async clock => {
            const running = failure(execute(['run', '--timeout', '60', target], runContext()));
            await printed('looping');
            clock.fire(60000);
            clock.fire(2000);
            const timedOut = await running;
            assert(timedOut?.exitCode === 124 && /stopped at its time limit/.test(timedOut.message), `The inherited time limit must stop ${target}: ${timedOut}`);
        });
    }
    assert(/react@19\.3\.1/.test(await execute(['list'], context)), 'The terminal must stay usable after a termination');
    assert(/completed/.test(await execute(['run', 'unawaited'], runContext())), 'A script that does not await its write must complete');
    assert(new TextDecoder().decode(await readFile('generated/script/unawaited.txt')) === 'done', 'A write the script did not await must be applied');
    await execute(['run', 'root'], runContext());
    assert(scriptOutput.includes('root ["generated/","tools/"]\r\n'), `The root must list the entries leading to granted paths: ${scriptOutput.filter(line => line.startsWith('root'))}`);
    await execute(['run', 'jsx'], runContext());
    assert(scriptOutput.includes('jsx b\r\n'), 'A TSX script must compile its JSX with the automatic runtime');
    // A stop during a large write: the file is either untouched or complete, and no staging is left.
    const writing = new AbortController();
    const big = failure(execute(['run', 'big'], runContext(writing.signal)));
    await printed('writing');
    writing.abort(new Error('Interrupted'));
    await big;
    const written = await readFile('generated/script/big.bin').then(bytes => bytes.length, () => 0);
    assert(written === 0 || written === 4 * 1024 * 1024, `A stopped write must leave no partial file: ${written} bytes`);
    const staged = JSON.parse(new TextDecoder().decode(await call('list', '.webfront/developer/staging')));
    assert(staged.length === 0, `A stopped write must leave no staged files: ${staged}`);
    // wf exec runs the webfront.bin entry of an installed package, never an npm bin.
    await execute(['install', 'webfront-fixture-tool', 'webfront-fixture-nodebin'], context);
    assert(/completed/.test(await execute(['exec', 'webfront-fixture-tool', 'hello'], runContext())), 'wf exec must run a WebFront entry');
    assert(/^webfront-fixture-lib@1\.\d+\.\d+ hello$/.test(new TextDecoder().decode(await readFile('generated/script/tool.txt'))), 'The entry must bundle its locked dependency');
    assert(/only declares an npm "bin"/.test((await failure(execute(['exec', 'webfront-fixture-nodebin'], runContext())))?.message), 'A package with only an npm bin must be refused');
    assert(/no WebFront executable entry/.test((await failure(execute(['exec', 'webfront-fixture-lib'], runContext())))?.message), 'A browser package without an entry must be refused');
    await progress('TypeScript checks');
    // wf check: a real TypeScript type check in a Worker, with the typescript and the declarations of the lock, all
    // served by the local registry.
    const checkRequests = [];
    globalThis.fetch = async (input, init) => {
        const url = new URL(input instanceof Request ? input.url : input, location.href).href;
        checkRequests.push(url);
        try { return await pageFetch(input, init); }
        catch (error) { throw new Error(`Registry request failed: ${url}: ${error.message}`, {cause: error}); }
    };
    const checkFiles = {
        'tsconfig.json': JSON.stringify({compilerOptions: {strict: true, target: 'ES2022', module: 'ESNext', moduleResolution: 'Bundler', lib: ['ES2022']}, include: ['check']}),
        'check/main.ts': "import {answer, area} from 'webfront-fixture-typed';\nimport {legacy} from 'webfront-fixture-legacy';\nimport {greet} from 'webfront-fixture-untyped';\nimport {twice} from './util';\n" +
            "export const total: number = twice(answer) + area({width: 2, height: 3});\nexport const word: string = greet(legacy);\n",
        'check/util.ts': 'export const twice = (value: number): number => 2 * value;\n',
    };
    for (const [path, text] of Object.entries(checkFiles)) await writeFile(path, text);
    const packageReads = new Map();
    const checkBridge = {socket: webFront.socket, cppFunction: name => name !== 'webfront.developer.read' ? webFront.cppFunction(name) :
        (path, offset) => {
            if (!path.startsWith('.webfront/developer/packages/')) return webFront.cppFunction(name)(path, offset);
            const key = `${path}:${offset}`;
            if (!packageReads.has(key)) {
                const reading = webFront.cppFunction(name)(path, offset).catch(error => {
                    packageReads.delete(key);
                    throw error;
                });
                packageReads.set(key, reading);
            }
            return packageReads.get(key);
        }};
    const retryFile = '.webfront/developer/packages/cache-retry-fixture/probe.txt';
    const retryRead = checkBridge.cppFunction('webfront.developer.read');
    assert(await retryRead(retryFile, 0).then(() => false, () => true), 'The first missing package read must fail');
    await writeFile(retryFile, 'recovered');
    assert(new TextDecoder().decode(await retryRead(retryFile, 0)) === 'recovered', 'A rejected package read must be retried');
    const checked = async (args = [], extra = {}) => {
        scriptOutput.length = 0;
        const result = await execute(['check', ...args], {...runContext(), bridge: checkBridge, ...extra}).then(text => ({text}), error => ({error}));
        return {...result, output: scriptOutput.join('')};
    };
    try {
        assert(/needs the typescript package: run wf install typescript@6/.test((await checked()).error?.message), 'wf check must ask for typescript');
        await progress('install TypeScript 4.9.5');
        await execute(['install', 'typescript@4.9.5'], context);
        const old = await checked();
        assert(/supports typescript 5\.x and 6\.x .* not typescript@4\.9\.5/.test(old.error?.message), `An unsupported typescript must be refused: ${old.error}`);
        await progress('install TypeScript 5.9.3 and declarations');
        await execute(['install', 'typescript@5.9.3', 'webfront-fixture-typed', 'webfront-fixture-legacy', 'webfront-fixture-untyped', '@types/webfront-fixture-untyped'], context);
        const five = await checked();
        assert(five.text && five.output.includes('No type errors in 2 project files (TypeScript 5.9.3).'), `TypeScript 5.9 must accept the project: ${five.error}\n${five.output}`);
        await progress('install TypeScript 6.0.3');
        await execute(['install', 'typescript@6.0.3'], context);
        const six = await checked();
        assert(/Type check completed/.test(six.text) && six.output.includes('No type errors in 2 project files (TypeScript 6.0.3).'), `TypeScript 6.0 must accept the project: ${six.error}\n${six.output}`);
        assert(checkRequests.length > 0 && checkRequests.every(url => url.startsWith(`${location.origin}/fixtures/registry/`)), `typescript must come from the local registry: ${checkRequests}`);
    } finally {
        globalThis.fetch = pageFetch;
    }
    await writeFile('check/util.ts', 'export const twice = (value: number): number => `${value}`;\n');
    const typeError = await checked();
    assert(typeError.error?.exitCode === 1 && typeError.output.includes("check/util.ts(1,49): error TS2322: Type 'string' is not assignable to type 'number'.") &&
        typeError.output.includes('Found 1 error in 1 file.'), `A type error must be reported with its file, line, column and code: ${typeError.error}\n${typeError.output}`);
    await writeFile('check/util.ts', checkFiles['check/util.ts']);
    const bounded = await checked([], {snapshotBounds: {files: 20, bytes: 64 * 1024 * 1024, entries: 1000}});
    assert(bounded.error?.exitCode === 1 && /the snapshot reached its bounds \(20 files, 64 MiB\): \d+ files were not loaded and are treated as missing/.test(bounded.output),
        `A snapshot past its bounds must be reported: ${bounded.error}\n${bounded.output}`);
    // NodeNext takes the module format from a package.json above the included directory, and composite keeps implying
    // declaration under noEmit.
    await writeFile('esm-check/package.json', JSON.stringify({type: 'module'}));
    await writeFile('esm-check/src/main.ts', 'export const value: number = await Promise.resolve(1);\n');
    await writeFile('tsconfig.json', JSON.stringify({compilerOptions: {strict: true, target: 'ES2022', module: 'NodeNext', moduleResolution: 'NodeNext',
        lib: ['ES2022'], composite: true, declarationMap: true, incremental: true}, include: ['esm-check/src']}));
    const esm = await checked();
    assert(esm.text && esm.output.includes('No type errors in 1 project file (TypeScript 6.0.3).'), `A valid NodeNext composite project must pass: ${esm.error}\n${esm.output}`);
    assert(await readFile('tsconfig.tsbuildinfo').then(() => false, () => true), 'An incremental check must not write build information');
    await writeFile('tsconfig.json', checkFiles['tsconfig.json']);
    // A long check is synchronous in its Worker: Ctrl+C terminates it after the grace period, and nothing is written.
    const slow = ['type Build<N extends number, T extends 0[] = []> = T["length"] extends N ? T : Build<N, [...T, 0]>;'];
    for (let n = 400; n < 1000; ++n) slow.push(`export const v${n}: Build<${n}>['length'] = ${n};`);
    await writeFile('check/slow.ts', slow.join('\n') + '\n');
    const listing = async path => new TextDecoder().decode(await call('list', path));
    const before = [await listing(''), await listing('check')];
    const stopCheck = new AbortController();
    scriptOutput.length = 0;
    const longCheck = failure(execute(['check'], {...runContext(stopCheck.signal), bridge: checkBridge}));
    await printed('Checking 3 root files');
    await withTimers([2000], async clock => {
        stopCheck.abort(new Error('Interrupted'));
        clock.fire(2000);
        const checkStopped = await longCheck;
        assert(checkStopped?.exitCode === 130 && /Type check stopped by Ctrl\+C \(terminated after the 2 s grace period\)/.test(checkStopped.message),
            `Ctrl+C must terminate a long check after the grace period: ${checkStopped}`);
    });
    assert(JSON.stringify([await listing(''), await listing('check')]) === JSON.stringify(before), 'A check must not write any project file');
    assert(JSON.parse(await listing('.webfront/developer/staging')).length === 0, 'A check must leave no staged files');
    assert(/typescript@6\.0\.3/.test(await execute(['list'], context)), 'The terminal must stay usable after a stopped check');
    // wf test: the specs of the manifest under the locked jasmine-core, in a frame of this page.
    const testOutput = [];
    let testRun = 0;
    await progress('Jasmine runs');
    const tested = async (args = [], extra = {}) => {
        const label = `wf test #${++testRun} ${args.join(' ')}`;
        const started = profileNow();
        await webFront.cppFunction('test.progress')(`${started.toFixed(3)}ms begin ${label}`);
        testOutput.length = 0;
        let report = null;
        const result = await execute(['test', ...args], {...runContext(), write: text => testOutput.push(text), onReport: value => { report = value; }, ...extra})
            .then(text => ({text}), error => ({error}));
        await webFront.cppFunction('test.progress')(`${profileNow().toFixed(3)}ms end ${label} durationMs=${(profileNow() - started).toFixed(3)} status=${report?.status ?? result.error?.message ?? 'done'}`);
        return {...result, report, output: testOutput.join('')};
    };
    const readManifest = async () => JSON.parse(new TextDecoder().decode(await readFile('webfront.project.json')));
    const specsOf = async specs => writeFile('webfront.project.json', JSON.stringify({...await readManifest(), test: {specs}}, null, 2));
    assert(/No specs: declare "test"/.test((await tested()).error?.message), 'wf test must ask for declared specs');
    await writeFile('tests/pass.spec.js', "describe('passing', () => {\n    it('adds', () => expect(1 + 1).toBe(2));\n    it('is pending', () => pending('not yet'));\n" +
        "    it('awaits', async () => { await Promise.resolve(); expect(true).toBeTrue(); });\n});\n");
    await specsOf(['tests/pass.spec.js']);
    assert(/jasmine-core is not installed: run wf install jasmine-core/.test((await tested()).error?.message), 'wf test must ask for jasmine-core');
    const testRequests = [];
    globalThis.fetch = (input, init) => {
        testRequests.push(new URL(input instanceof Request ? input.url : input, location.href).href);
        return pageFetch(input, init);
    };
    try { await execute(['install', 'jasmine-core'], context); } finally { globalThis.fetch = pageFetch; }
    assert(testRequests.length > 0 && testRequests.every(url => url.startsWith(`${location.origin}/fixtures/registry/`)), `jasmine-core must come from the local registry: ${testRequests}`);
    const testLock = await runtime.readLock(webFront);
    const jasmineVersion = testLock.packages[testLock.roots['jasmine-core'].id].version;
    const passing = await tested();
    assert(passing.text && passing.report?.status === 'passed' && passing.report.exitCode === 0 && passing.report.total === 3 && passing.report.passed === 2 &&
        passing.report.pending === 1 && passing.report.failed === 0 && passing.report.jasmine === jasmineVersion,
        `Passing specs must be reported exactly: ${passing.error}\n${JSON.stringify(passing.report)}`);
    assert(passing.output.startsWith('.*.') && passing.text.startsWith('PASSED: 3 specs, 0 failures, 1 pending') && /PENDING passing is pending: not yet/.test(passing.text) &&
        passing.text.includes(`jasmine-core ${jasmineVersion}`), `The terminal must show the progress and the summary: ${passing.output}\n${passing.text}`);
    assert(!document.querySelector('iframe[title="WebFront tests"]'), 'The runner frame must be removed after a run');
    // A failing spec and the asynchronous failures: a late exception, an unhandled rejection, a spec reaching the bridge.
    await writeFile('tests/fail.spec.ts', "describe('failing', () => {\n    it('fails with a message', () => { const expected: number = 1; expect(expected).toBe(2); });\n});\n");
    await writeFile('tests/async.spec.js', "describe('asynchronous', () => {\n    it('throws later', done => { setTimeout(() => { throw new Error('late boom'); }, 0); setTimeout(done, 80); });\n" +
        "    it('rejects unhandled', async () => { Promise.reject(new Error('unhandled boom')); await new Promise(resolve => setTimeout(resolve, 80)); });\n" +
        "    it('reaches the bridge', async () => expect(await webFront.cppFunction('test.browsers')()).toBeGreaterThan(0));\n});\n");
    await specsOf(['tests/pass.spec.js', 'tests/fail.spec.ts', 'tests/async.spec.js']);
    const failing = await tested();
    const failedNames = failing.report?.failures.map(failure => failure.name);
    assert(failing.error?.exitCode === 1 && failing.report?.status === 'failed' && failing.report.exitCode === 1 && failing.report.total === 7 && failing.report.passed === 3 &&
        failing.report.failed === 3 && failing.report.pending === 1, `A failing run must report exact counts and exit 1: ${failing.error}\n${JSON.stringify(failing.report)}`);
    assert(JSON.stringify(failedNames) === JSON.stringify(['failing fails with a message', 'asynchronous throws later', 'asynchronous rejects unhandled']), `The failed specs must be named in order: ${failedNames}`);
    const messageOf = name => failing.report.failures.find(failure => failure.name.endsWith(name)).messages.map(message => message.message).join('\n');
    assert(/Expected 1 to be 2\./.test(messageOf('fails with a message')) && /late boom/.test(messageOf('throws later')) && /unhandled boom/.test(messageOf('rejects unhandled')),
        `Failure messages must be exact: ${JSON.stringify(failing.report.failures)}`);
    assert(failing.error.message.startsWith('FAILED: 7 specs, 3 failures, 1 pending') && failing.error.message.includes('FAIL failing fails with a message\n  Expected 1 to be 2.') &&
        failing.output.startsWith('.*.F'), `The terminal must show the failures: ${failing.output}\n${failing.error.message}`);
    // A pattern keeps the specs whose full name contains it; one that matches nothing is not a pass.
    const filtered = await tested(['passing']);
    assert(filtered.report?.status === 'passed' && filtered.report.total === 3 && filtered.report.excluded === 4 && /4 filtered out/.test(filtered.text), `A pattern must filter the specs: ${filtered.error}\n${JSON.stringify(filtered.report)}`);
    const unmatched = await tested(['no such spec']);
    assert(unmatched.error?.exitCode === 1 && unmatched.report?.status === 'incomplete' && /No spec matches "no such spec"/.test(unmatched.error.message), `A pattern matching nothing must fail: ${unmatched.error}`);
    // An afterAll that throws is reported with its suite; a stylesheet a spec imports is applied to its page.
    await writeFile('tests/styled.css', '.probe { display: block; width: 37px; }\n');
    await writeFile('tests/suite.spec.js', "import './styled.css';\ndescribe('cleanup', () => {\n    it('passes', () => {});\n    afterAll(() => { throw new Error('afterAll boom'); });\n});\n" +
        "describe('styled', () => {\n    it('applies the imported stylesheet', () => {\n        const probe = document.createElement('div');\n        probe.className = 'probe';\n        document.body.append(probe);\n        expect(getComputedStyle(probe).width).toBe('37px');\n        probe.remove();\n    });\n});\n");
    await specsOf(['tests/suite.spec.js']);
    const suiteFailure = await tested();
    assert(suiteFailure.error?.exitCode === 1 && suiteFailure.report?.status === 'failed' && suiteFailure.report.total === 2 && suiteFailure.report.passed === 2 && suiteFailure.report.failed === 0 &&
        suiteFailure.report.suites.length === 1 && suiteFailure.report.suites[0].name === 'cleanup' && /afterAll boom/.test(suiteFailure.report.suites[0].messages[0].message) &&
        /FAIL cleanup \(suite\)/.test(suiteFailure.error.message), `An afterAll failure must be reported with its suite, and the stylesheet applied: ${suiteFailure.error}\n${JSON.stringify(suiteFailure.report)}`);
    // A spec that does not load fails the run: it is a build error for one that does not compile, a report for one that throws.
    await writeFile('tests/missing.spec.js', "import './absent';\n");
    await specsOf(['tests/pass.spec.js', 'tests/missing.spec.js']);
    const unbuilt = await tested();
    assert((unbuilt.error?.exitCode ?? 1) === 1 && /absent/.test(unbuilt.error.message) && !unbuilt.report, `An unresolved import must fail the build: ${unbuilt.error}`);
    await writeFile('tests/throws.spec.js', "throw new Error('loading boom');\n");
    await specsOf(['tests/throws.spec.js']);
    const unloaded = await tested();
    assert(unloaded.error?.exitCode === 1 && unloaded.report?.status !== 'passed' && /loading boom/.test(unloaded.error.message), `A spec that throws while loading must fail: ${unloaded.error}\n${JSON.stringify(unloaded.report)}`);
    // A time limit and Ctrl+C end a run that does not return, remove the frame and report it.
    const hangingToken = crypto.randomUUID();
    await writeFile('tests/hang.spec.js', `describe('hanging', () => {\n    it('never ends', () => {\n` +
        `        parent.postMessage({hangingSpec: ${JSON.stringify(hangingToken)}}, location.origin);\n` +
        "        return new Promise(() => {});\n    }, 600000);\n});\n");
    await specsOf(['tests/hang.spec.js']);
    const hangingStarted = () => {
        let receive;
        const ready = new Promise(resolve => {
            receive = event => {
                const frame = document.querySelector('iframe[title="WebFront tests"]');
                if (event.origin === location.origin && event.source === frame?.contentWindow && event.data?.hangingSpec === hangingToken)
                    resolve();
            };
            window.addEventListener('message', receive);
        });
        return {ready, dispose: () => window.removeEventListener('message', receive)};
    };
    // Wait for the spec itself: removing a merely inserted frame can interrupt its navigation instead of its run.
    await withTimers([60000], async clock => {
        const started = hangingStarted();
        try {
            const hanging = tested(['--timeout', '60']);
            await untilReady(started.ready, hanging);
            assert(clock.pending() === 1, 'Only the frame deadline must remain after preparation');
            clock.fire(60000);
            const hung = await hanging;
            assert(hung.error?.exitCode === 124 && hung.report?.status === 'timeout' && /time limit/.test(hung.error.message), `A hanging run must stop at its time limit: ${hung.error}`);
            assert(!document.querySelector('iframe[title="WebFront tests"]'), 'A timed out run must remove its frame');
        } finally { started.dispose(); }
    });
    const stopTests = new AbortController();
    const started = hangingStarted();
    try {
        const interrupted = tested([], {signal: stopTests.signal});
        await untilReady(started.ready, interrupted);
        stopTests.abort(new Error('Interrupted'));
        const interruptedRun = await interrupted;
        assert(interruptedRun.error?.exitCode === 130 && interruptedRun.report?.status === 'interrupted' && !document.querySelector('iframe[title="WebFront tests"]'), `Ctrl+C must stop a run: ${interruptedRun.error}`);
    } finally { started.dispose(); }
    // The command must stop even when the bridge read never settles. Wait for the read, not wall time.
    const blockedRead = () => {
        let started;
        const ready = new Promise(resolve => { started = resolve; });
        const bridge = {socket: webFront.socket, cppFunction: name => name === 'webfront.developer.read' ?
            () => { started(); return new Promise(() => {}); } : webFront.cppFunction(name)};
        return {bridge, ready};
    };
    await specsOf(['tests/pass.spec.js']);
    await withTimers([60000], async clock => {
        const blocked = blockedRead();
        const running = tested(['--timeout', '60'], {bridge: blocked.bridge});
        await untilReady(blocked.ready, running);
        clock.fire(60000);
        const result = await running;
        assert(result.error?.exitCode === 124 && result.report?.status === 'timeout', `A blocked bridge must report the preparation timeout: ${result.error}`);
    });
    const blocked = blockedRead();
    const interruptPreparing = new AbortController();
    const preparingRun = tested([], {bridge: blocked.bridge, signal: interruptPreparing.signal});
    await untilReady(blocked.ready, preparingRun);
    interruptPreparing.abort(new Error('Interrupted'));
    const preparingResult = await preparingRun;
    assert(preparingResult.error?.exitCode === 130 && preparingResult.report?.status === 'interrupted', `Ctrl+C during preparation must end with 130 and a report: ${preparingResult.error}`);
    // macOS 13.3 supports WKWebView, but its WebKit need not provide AbortSignal.any (Safari 17.4).
    const signalAny = Object.getOwnPropertyDescriptor(AbortSignal, 'any');
    Object.defineProperty(AbortSignal, 'any', {configurable: true, value: undefined});
    try {
        const legacyPassing = await tested([], {signal: new AbortController().signal});
        assert(legacyPassing.report?.status === 'passed' && legacyPassing.report.exitCode === 0,
            `A terminal run must work without AbortSignal.any: ${legacyPassing.error}`);
        const alreadyStopped = new AbortController();
        alreadyStopped.abort(new Error('Interrupted'));
        const legacyStopped = await tested([], {signal: alreadyStopped.signal});
        assert(legacyStopped.error?.exitCode === 130 && legacyStopped.report?.status === 'interrupted',
            `An already aborted signal must report interruption without AbortSignal.any: ${legacyStopped.error}`);
        await withTimers([60000], async clock => {
            const blocked = blockedRead();
            const running = tested(['--timeout', '60'], {bridge: blocked.bridge, signal: new AbortController().signal});
            await untilReady(blocked.ready, running);
            clock.fire(60000);
            const result = await running;
            assert(result.error?.exitCode === 124 && result.report?.status === 'timeout',
                `The preparation deadline must report timeout without AbortSignal.any: ${result.error}`);
        });
        const legacyStop = new AbortController();
        const blocked = blockedRead();
        const legacyPreparing = tested([], {bridge: blocked.bridge, signal: legacyStop.signal});
        await untilReady(blocked.ready, legacyPreparing);
        legacyStop.abort(new Error('Interrupted'));
        const legacyInterrupted = await legacyPreparing;
        assert(legacyInterrupted.error?.exitCode === 130 && legacyInterrupted.report?.status === 'interrupted',
            `Ctrl+C during preparation must report interruption without AbortSignal.any: ${legacyInterrupted.error}`);
    } finally {
        if (signalAny) Object.defineProperty(AbortSignal, 'any', signalAny);
        else delete AbortSignal.any;
    }
    const withScript = await readManifest();
    await writeFile('webfront.project.json', JSON.stringify({...withScript, scripts: {...withScript.scripts, specs: 'wf test --timeout 120'}}, null, 2));
    await withTimers([60000], async clock => {
        let started;
        const ready = new Promise(resolve => { started = resolve; });
        let reads = 0;
        const bridge = {socket: webFront.socket, cppFunction: name => name === 'webfront.developer.read' ?
            (...args) => {
                // The first manifest/lock pair belongs to wf run; block preparation of its nested wf test.
                if (++reads > 2) { started(); return new Promise(() => {}); }
                return webFront.cppFunction(name)(...args);
            } : webFront.cppFunction(name)};
        const running = execute(['run', '--timeout', '60', 'specs'], {...runContext(), bridge, onReport: () => {}}).then(() => null, error => error);
        await untilReady(ready, running);
        clock.fire(60000);
        const inherited = await running;
        assert(inherited?.exitCode === 124, `wf test must keep the deadline of the script that runs it: ${inherited}`);
    });
    // Each run publishes its own directory, emptied at the end: runs of two sessions never share files.
    const runDirectories = JSON.parse(new TextDecoder().decode(await call('list', '.webfront/developer/test')));
    assert(runDirectories.length > 1 && runDirectories.every(name => /^[0-9a-f-]{36}\/?$/.test(name)), `Each run must have its own directory: ${runDirectories}`);
    for (const name of runDirectories) assert(JSON.parse(new TextDecoder().decode(await call('list', `.webfront/developer/test/${name.replace(/\/$/, '')}`))).length === 0, `The files of a finished run must be removed: ${name}`);
    assert(/--timeout expects whole seconds/.test((await tested(['--timeout', '0'])).error?.message) && /Usage: wf test/.test((await tested(['a', 'b'])).error?.message), 'wf test must check its options');
    // Without a terminal: the result reaches C++ through the bridge and the session ends with the command's status.
    await specsOf(['tests/pass.spec.js', 'tests/fail.spec.ts']);
    const exits = [];
    const sent = await nonInteractive(['test'], {...runContext(), reporter: json => webFront.cppFunction('test.result')(json), onExit: code => exits.push(code)});
    const received = JSON.parse(await webFront.cppFunction('test.lastResult')());
    assert(sent.exitCode === 1 && exits.length === 1 && exits[0] === 1 && received.exitCode === 1 && received.report?.failed === 1 && received.report.total === 4 &&
        received.report.failures[0].name === 'failing fails with a message' && received.output.includes('FAILED: 4 specs, 1 failure'), `A non-interactive failure must reach C++ and end the session with 1: ${JSON.stringify(received)}`);
    await specsOf(['tests/pass.spec.js']);
    await nonInteractive(['test'], {...runContext(), reporter: json => webFront.cppFunction('test.result')(json), onExit: code => exits.push(code)});
    const success = JSON.parse(await webFront.cppFunction('test.lastResult')());
    assert(exits[1] === 0 && success.exitCode === 0 && success.report.status === 'passed', `A non-interactive success must end the session with 0: ${JSON.stringify(success)}`);
    // Four 1500-NUL messages take at least 72000 JSON bytes across output + report, independently of stack format.
    // This crosses the 65535-byte bridge limit with much smaller raw strings and preserves all four failures.
    await writeFile('tests/big.spec.js', "describe('big', () => {\n    for (let index = 0; index < 4; ++index) it('fails with a long message ' + index, () => { throw new Error('\\0'.repeat(1500)); });\n});\n");
    await specsOf(['tests/big.spec.js']);
    await nonInteractive(['test'], {...runContext(), reporter: json => webFront.cppFunction('test.result')(json), onExit: code => exits.push(code)});
    const cut = JSON.parse(await webFront.cppFunction('test.lastResult')());
    assert(exits[2] === 1 && cut.exitCode === 1 && cut.truncated === true && cut.report?.failed === 4 && cut.report.total === 4 && cut.report.failures.length === 4,
        `An oversized result must be cut to cross the bridge: ${JSON.stringify(cut).slice(0, 300)}`);
    exits.splice(2, 1);
    await nonInteractive(['test', '--timeout', 'x'], {...runContext(), reporter: () => { throw new Error('bridge down'); }, onExit: code => exits.push(code)});
    assert(exits[2] === 1, 'A command that fails and a result that cannot be sent must both end non-zero');
    await nonInteractive(['list'], {...runContext(), reporter: () => { throw new Error('bridge down'); }, onExit: code => exits.push(code)});
    assert(exits[3] === 1, 'A result that cannot be sent must not end the session with 0');
    await progress('terminal and sessions');
    // The terminal: the application's C++ commands, run in this browser's session.
    assert(JSON.stringify([...runtime.builtinNames].sort()) === JSON.stringify([...runtime.reservedNames].sort()),
        `The terminal's own commands must be the names reserved to it: ${runtime.builtinNames}`);
    const cppCommands = await runtime.discoverCommands(webFront);
    assert(['echo', 'fail', 'linger', 'status', 'stream', 'wait'].every(name => cppCommands.has(name)) && cppCommands.get('echo') === 'Write its arguments',
        `The C++ commands must be discovered with their help: ${[...cppCommands.keys()]}`);
    const commandState = tag => webFront.cppFunction('test.command')(tag);
    const waitState = async (tag, state) => {
        for (let attempt = 0; (await commandState(tag)) !== state; ++attempt) {
            if (attempt === 200) throw new Error(`Command ${tag} did not reach ${state}: ${await commandState(tag)}`);
            await delay(25);
        }
    };
    const lineContext = (signal = new AbortController().signal, chunks = []) => ({...context, cppCommands, signal, write: text => chunks.push(text)});
    const streamed = [];
    assert(await runtime.runLine('stream', lineContext(undefined, streamed)) === '', 'A C++ command resolves once it returns');
    assert(streamed.length >= 3 && streamed.join('') === 'é'.repeat(75000) + '\r\n', `Long C++ output must stream in pieces under the bridge limit: ${streamed.length}`);
    const crlf = [];
    await runtime.runLine('crlf', lineContext(undefined, crlf));
    assert(crlf.join('') === 'one\r\ntwo\r\n', `A CRLF split between two writes must stay one line break: ${JSON.stringify(crlf.join(''))}`);
    const thrown = await failure(runtime.runLine('fail', lineContext()));
    assert(thrown?.message === 'command failed on purpose' && thrown.exitCode === 1, `A C++ exception must reject the command: ${thrown}`);
    const exitCode = await failure(runtime.runLine('status 3', lineContext()));
    assert(exitCode?.exitCode === 3 && /exited with code 3/.test(exitCode.message), `A C++ exit code must be reported: ${exitCode}`);
    assert(/Unknown command: absent/.test((await failure(runtime.runLine('absent', lineContext())))?.message), 'An unknown command must be refused');
    // Ctrl+C: the terminal stops waiting at once, the next command answers while the first is still ending in C++.
    const interruptLinger = new AbortController();
    const lingering = failure(runtime.runLine('linger first', lineContext(interruptLinger.signal)));
    await waitState('first', 'running');
    interruptLinger.abort(new Error('Interrupted'));
    assert((await lingering)?.exitCode === 130, 'An interrupted C++ command must end with exit code 130');
    const echoed = [];
    await runtime.runLine('echo "after Ctrl+C"', lineContext(undefined, echoed));
    assert(echoed.join('') === 'after Ctrl+C\r\n', `The next command must run normally: ${echoed}`);
    assert(['running', 'stopping'].includes(await commandState('first')), 'The second command must answer before the first one ends');
    await waitState('first', 'stopping');
    await webFront.cppFunction('test.release')('first');
    await waitState('first', 'cancelled');
    // Sessions: a second browser has its own commands; a long command there does not delay this one, Ctrl+C here does
    // not stop it, and its disconnection does.
    const sessionFrame = document.createElement('iframe');
    sessionFrame.src = '/fixtures/session.html';
    document.body.append(sessionFrame);
    for (let attempt = 0; !sessionFrame.contentWindow?.session; ++attempt) {
        if (attempt === 200) throw new Error('The second session did not load');
        await delay(25);
    }
    const second = await sessionFrame.contentWindow.session;
    const secondWait = failure(second.runtime.runLine('wait second', {bridge: second.bridge, cppCommands: second.cppCommands, signal: new AbortController().signal}));
    await waitState('second', 'running');
    const interruptFirst = new AbortController();
    const firstWait = failure(runtime.runLine('wait here', lineContext(interruptFirst.signal)));
    await waitState('here', 'running');
    const echoStarted = performance.now();
    await runtime.runLine('echo isolated', lineContext());
    assert(performance.now() - echoStarted < 2000, 'A command in another session must not delay this one');
    interruptFirst.abort(new Error('Interrupted'));
    await firstWait;
    await waitState('here', 'cancelled');
    assert(await commandState('second') === 'running', 'Ctrl+C in one browser must not stop the command of another');
    const browsers = await webFront.cppFunction('test.browsers')();
    // Its connection closes while its acknowledged command runs: the terminal stops waiting for an exit that cannot come.
    second.bridge.socket.close();
    const lost = await secondWait;
    assert(lost?.exitCode === 1 && /connection to the application closed/.test(lost.message),
        `A command of a closed connection must reject: ${lost?.message}`);
    await waitState('second', 'cancelled');
    for (let attempt = 0; await webFront.cppFunction('test.browsers')() !== browsers - 1; ++attempt)
        if (attempt === 200) throw new Error('The connection of a closed browser must be released');
        else await delay(25);
    sessionFrame.remove();
    // The terminal itself, mounted in this page with a real xterm.js.
    const host = document.createElement('div');
    host.style.cssText = 'width:640px;height:240px';
    document.body.append(host);
    let exitedWith = null;
    // The test sets the terminal's size itself: an observer following the element would race with it.
    const mounted = async () => runtime.mount({bridge: webFront, config, terminal: host, preview: context.preview, onExit: code => { exitedWith = code; }, fitToElement: false});
    const {terminal, editor} = await mounted();
    terminal.resize(80, 12);
    const buffer = () => terminal.buffer.active;
    const rendered = () => new Promise(resolve => terminal.write('', resolve));
    // Like xterm.js, one input per key; text may come as one input.
    const type = async (...inputs) => {
        for (const data of inputs) {
            terminal.input(data);
            await editor.whenIdle();
        }
        await rendered();
    };
    const rows = () => {
        const lines = [];
        for (let y = 0; y < buffer().length; ++y) lines.push(buffer().getLine(y).translateToString(true));
        return lines;
    };
    const cursorRow = () => buffer().baseY + buffer().cursorY;
    const lastRows = count => rows().slice(cursorRow() - count + 1, cursorRow() + 1);
    await type('echo hello\r');
    assert(JSON.stringify(lastRows(3)) === JSON.stringify(['> echo hello', 'hello', '> ']), `A C++ command must run from the terminal: ${lastRows(3)}`);
    // Cell widths: CJK, emoji, a flag, a ZWJ sequence and a combining mark.
    await type('漢字😀🇫🇷👨‍👩‍👧éx');
    assert(buffer().cursorX === 2 + 2 + 2 + 2 + 2 + 2 + 1 + 1, `Wide characters must take two cells: ${buffer().cursorX}`);
    // xterm.js draws each cluster where the editor placed it.
    const drawn = buffer().getLine(cursorRow());
    const cellsDrawn = [[2, '漢', 2], [4, '字', 2], [6, '😀', 2], [8, '🇫🇷', 2], [10, '👨‍👩‍👧', 2], [12, 'e\u0301', 1], [13, 'x', 1]]
        .map(([column, chars, width]) => [drawn.getCell(column).getChars() === chars, drawn.getCell(column).getWidth() === width]).flat();
    assert(cellsDrawn.every(Boolean), `xterm.js must draw the clusters with the editor's widths: ${cellsDrawn}`);
    await type('\x7f', '\x7f');
    assert(buffer().cursorX === 12 && editor.line === '漢字😀🇫🇷👨‍👩‍👧', `Backspace must erase whole clusters: ${buffer().cursorX} ${editor.line}`);
    await type('\x1b[D', '\x1b[D');
    assert(buffer().cursorX === 8, `The cursor must move over whole clusters: ${buffer().cursorX}`);
    await type('\x03');
    assert(lastRows(2)[0].endsWith('^C') && lastRows(1)[0] === '> ' && editor.line === '', 'Ctrl+C must drop the line');
    // A line over several rows: wrapping, editing in the middle, a row filled exactly and a wide character at a row end.
    terminal.resize(20, 12);
    await rendered();
    assert(terminal.cols === 20, `The terminal must keep the width the test gives it: ${terminal.cols}`);
    await type('echo ' + 'a'.repeat(40));
    let top = cursorRow() - 2;
    assert(buffer().cursorX === 7 && JSON.stringify(rows().slice(top, top + 3)) === JSON.stringify(['> echo ' + 'a'.repeat(13), 'a'.repeat(20), 'a'.repeat(7)]),
        `A long line must wrap: ${rows().slice(top, top + 3)}`);
    await type('\x01', 'X');
    assert(cursorRow() === top && buffer().cursorX === 3 && JSON.stringify(rows().slice(top, top + 3)) === JSON.stringify(['> Xecho ' + 'a'.repeat(12), 'a'.repeat(20), 'a'.repeat(8)]),
        `Editing the start of a long line must redraw its rows: ${rows().slice(top, top + 3)}`);
    await type('\x05', 'b'.repeat(12));
    assert(cursorRow() === top + 3 && buffer().cursorX === 0, `A line filling its last row exactly must leave the cursor on the next row: ${cursorRow() - top} ${buffer().cursorX}`);
    await type('\x7f', '漢');
    assert(cursorRow() === top + 3 && buffer().cursorX === 2 && rows()[top + 2] === 'a'.repeat(8) + 'b'.repeat(11) && rows()[top + 3] === '漢',
        `A wide character must not split at a row end: ${buffer().cursorX} ${rows().slice(top + 2, top + 4)}`);
    await type('\x15');
    assert(cursorRow() === top && buffer().cursorX === 2 && rows()[top + 1] === '', 'Erasing a long line must clear its rows');
    terminal.resize(80, 12);
    await rendered();
    // Exit codes, errors and clear.
    await type('status 3\r');
    assert(editor.status === 3 && lastRows(2)[0] === 'Error: Command status exited with code 3', `An exit code must be shown: ${lastRows(2)}`);
    await type('fail\r');
    assert(editor.status === 1 && lastRows(2)[0] === 'Error: command failed on purpose', `A C++ exception must be shown as an error: ${lastRows(2)}`);
    await type('partial\r', 'x');
    assert(JSON.stringify(lastRows(2)) === JSON.stringify(['partial', '> x']), `Output without a final line break must not hold the prompt: ${lastRows(2)}`);
    await type('\x03');
    // Keys typed while a completion is pending are applied after it, Enter included.
    terminal.input('ec');
    terminal.input('\t');
    terminal.input('ho');
    terminal.input(' typed-ahead\r');
    for (let attempt = 0; editor.busy || lastRows(2)[0] !== 'ho typed-ahead'; ++attempt) {
        if (attempt === 200) throw new Error(`Keys typed during a completion must be kept: ${lastRows(3)}`);
        await editor.whenIdle();
        await rendered();
        await delay(10);
    }
    assert(lastRows(3)[0] === '> echo ho typed-ahead' && editor.ahead.length === 0, `The completed line must run once: ${lastRows(3)}`);
    await type('echo ok\r');
    assert(editor.status === 0, 'A successful command must reset the status');
    // Ctrl+C in the terminal: the prompt comes back at once, and a command typed then runs while the first one ends.
    terminal.input('linger terminal\r');
    await waitState('terminal', 'running');
    await type('\x03');
    await type('echo next\r');
    const lingerState = await commandState('terminal');
    assert(editor.status === 0 && lastRows(2)[0] === 'next' && ['running', 'stopping'].includes(lingerState), `Ctrl+C must interrupt a C++ command: ${lastRows(3)} ${editor.status} ${lingerState}`);
    await waitState('terminal', 'stopping');
    await webFront.cppFunction('test.release')('terminal');
    // Pasted lines run one after the other; the last one, without a line break, waits on the prompt.
    // Keys typed one by one while a command runs are applied after it, Enter included.
    terminal.input('sleep 300\r');
    for (const key of [...'echo ahead', '\r']) terminal.input(key);
    await editor.whenIdle();
    await rendered();
    assert(JSON.stringify(lastRows(4)) === JSON.stringify(['> sleep 300', '> echo ahead', 'ahead', '> ']), `Keys typed during a command must be kept: ${lastRows(4)}`);
    terminal.paste('echo one\necho two\necho three');
    await editor.whenIdle();
    await rendered();
    assert(JSON.stringify(lastRows(5)) === JSON.stringify(['> echo one', 'one', '> echo two', 'two', '> echo three']), `A multi-line paste must run line by line: ${lastRows(5)}`);
    await type('\r');
    assert(lastRows(2)[0] === 'three', 'The pasted last line must run once submitted');
    // Completion of command names, wf subcommands, manifest scripts and project paths.
    const completed = async (text, expected) => {
        await type(text, '\t');
        assert(editor.line === expected, `${JSON.stringify(text)} must complete to ${JSON.stringify(expected)}: ${JSON.stringify(editor.line)}`);
        await type('\x03');
    };
    await completed('ec', 'echo ');
    await completed('wf ru', 'wf run ');
    await completed('wf run ge', 'wf run gen ');
    await completed('echo too', 'echo tools/');
    await completed('echo tools/js', 'echo tools/jsx.tsx ');
    await completed('hi', 'history ');
    await completed('echo /to', 'echo /to');  // project paths are relative: an absolute one is not completed
    // Paths with spaces and quotes: the completion reads the escaped or quoted word and extends it the same way.
    await writeFile("my dir/file one's.txt", 'x');
    await completed('echo my\\ d', 'echo my\\ dir/');
    await completed('echo my\\ dir/f', "echo my\\ dir/file\\ one\\'s.txt ");
    await completed('echo "my d', 'echo "my dir/');
    await completed('echo "my dir/f', `echo "my dir/file one's.txt" `);
    // A single quote cannot be written between single quotes: nothing is inserted.
    await completed("echo 'my dir/f", "echo 'my dir/f");
    await type('h', '\t');
    assert(editor.line === 'h' && lastRows(2)[0].includes('help') && lastRows(2)[0].includes('history'), `Several candidates must be listed: ${lastRows(2)}`);
    await type('\x03');
    await type('clear\r');
    assert(buffer().length === terminal.rows && rows()[0] === '> ', `clear must empty the screen and the scrollback: ${rows()[0]}`);
    // A line taller than the screen is drawn a screen at a time around the cursor, never duplicated on it.
    terminal.resize(20, 4);
    await rendered();
    const digits = '0123456789'.repeat(10).slice(0, 95);
    const tall = '> echo ' + digits;
    const layoutRow = index => tall.slice(20 * index, 20 * index + 20);
    const screen = () => rows().slice(buffer().baseY, buffer().baseY + terminal.rows);
    const shows = (first, cursorY, cursorX, message) => {
        const expected = [0, 1, 2, 3].map(index => layoutRow(first + index));
        assert(JSON.stringify(screen()) === JSON.stringify(expected) && buffer().cursorY === cursorY && buffer().cursorX === cursorX,
            `${message}: ${JSON.stringify(screen())} at ${buffer().cursorY},${buffer().cursorX}`);
    };
    await type('echo ' + digits);
    shows(2, 3, 2, 'The end of a tall line must be on screen');
    await type('\x01');
    shows(0, 0, 2, 'Home must show the start of a tall line, once');
    await type('\x05');
    shows(2, 3, 2, 'End must show the end of a tall line again');
    await type('\r');
    assert(lastRows(2)[0] === digits.slice(80) && lastRows(1)[0] === '> ', `A tall line must run: ${screen()}`);
    terminal.resize(80, 12);
    await type('clear\r');
    // Enter and Ctrl+C typed while a completion is pending: the command starts, then is interrupted at once.
    terminal.input('lin');
    terminal.input('\t');
    terminal.input('replay');
    terminal.input('\r');
    terminal.input('\x03');
    await waitState('replay', 'stopping');
    await webFront.cppFunction('test.release')('replay');
    for (let attempt = 0; editor.busy; ++attempt) {
        if (attempt === 100) throw new Error(`Ctrl+C typed during a completion must interrupt the replayed command: ${lastRows(3)}`);
        await delay(20);
    }
    assert(editor.status === 130 && lastRows(3)[0] === '> linger replay', `The replayed command must be interrupted: ${lastRows(3)} ${editor.status}`);
    // The history is kept per project: a new terminal recalls it.
    await runtime.historySaved();
    const saved = JSON.parse(new TextDecoder().decode(await readFile(runtime.historyPath)));
    assert(saved.at(-1) === 'linger replay' && saved.includes('clear') && saved.includes('echo three'), `The history must be saved in the project: ${saved.slice(-3)}`);
    await type('exit 4\r');
    assert(exitedWith === 4 && editor.closed && lastRows(2)[0] === 'Session ended. Press Enter to start a new session.', `exit must end the session with its code: ${exitedWith}`);
    await type('echo ignored');
    assert(!editor.busy && editor.line === '', 'A closed session must ignore input');
    // The window stays open when the browser refuses to close it: Enter starts a new session.
    await type('\r');
    assert(!editor.closed && editor.status === 0 && lastRows(1)[0] === '> ', `Enter must start a new session after exit: ${lastRows(2)}`);
    await runtime.historySaved();
    terminal.dispose();
    host.replaceChildren();
    const again = await mounted();
    again.terminal.input('\x1b[A');
    await new Promise(resolve => again.terminal.write('', resolve));
    assert(again.editor.line === 'exit 4', `A new terminal must recall the project's history: ${again.editor.line}`);
    again.terminal.input('\x03');
    // Two terminals on the same project: each one adds its lines to the history, neither erases the other's.
    const otherHost = document.createElement('div');
    otherHost.style.cssText = 'width:640px;height:240px';
    document.body.append(otherHost);
    const other = await runtime.mount({bridge: webFront, config, terminal: otherHost, preview: context.preview, onExit: () => {}});
    for (const [session, line] of [[again, 'echo from-first'], [other, 'echo from-second'], [again, 'echo first-again']]) {
        session.terminal.input(line + '\r');
        await session.editor.whenIdle();
    }
    await runtime.historySaved();
    const shared = JSON.parse(new TextDecoder().decode(await readFile(runtime.historyPath)));
    assert(JSON.stringify(shared.slice(-3)) === JSON.stringify(['echo from-first', 'echo from-second', 'echo first-again']),
        `Terminals must not erase each other's history: ${shared.slice(-4)}`);
    other.terminal.dispose();
    otherHost.remove();
    // Two pages, each with its own runtime module, write their histories at the same time: no line is lost.
    const historyFrame = document.createElement('iframe');
    historyFrame.src = '/fixtures/session.html';
    document.body.append(historyFrame);
    for (let attempt = 0; !historyFrame.contentWindow?.session; ++attempt) {
        if (attempt === 200) throw new Error('The second page did not load');
        await delay(25);
    }
    const page = await historyFrame.contentWindow.session;
    const pageTerminal = await page.runtime.mount({bridge: page.bridge, config, terminal: historyFrame.contentDocument.getElementById('terminal'),
        preview: context.preview, onExit: () => {}, fitToElement: false});
    const lines = page => [1, 2, 3, 4, 5].map(index => `echo ${page}${index}`);
    again.terminal.paste(lines('here').join('\n') + '\n');
    pageTerminal.terminal.paste(lines('there').join('\n') + '\n');
    await Promise.all([again.editor.whenIdle(), pageTerminal.editor.whenIdle()]);
    await Promise.all([runtime.historySaved(), page.runtime.historySaved()]);
    const merged = JSON.parse(new TextDecoder().decode(await readFile(runtime.historyPath)));
    const missing = [...lines('here'), ...lines('there')].filter(line => !merged.includes(line));
    assert(missing.length === 0, `Concurrent pages must keep every history line: ${missing}`);
    // A history that cannot be read is never replaced by the lines of one terminal.
    await writeFile(runtime.historyPath, '{broken');
    again.terminal.input('echo kept\r');
    await again.editor.whenIdle();
    await runtime.historySaved();
    assert(new TextDecoder().decode(await readFile(runtime.historyPath)) === '{broken', 'A malformed history must be left unchanged');
    pageTerminal.terminal.dispose();
    historyFrame.remove();
    again.terminal.dispose();
    host.remove();
    // The application stops while this command runs: its shutdown must drain it (checked in C++).
    void failure(runtime.runLine('wait final', lineContext()));
    await waitState('final', 'running');
    await webFront.cppFunction('test.report')(true, 'WASM + npm + TSX + Life animation + deterministic headers + scripts + type check + terminal passed');
} catch (error) {
    if (error instanceof PublicSmokeDone)
        await webFront.cppFunction('test.report')(true, 'Public distribution: WASM + npm + TSX + Life animation + deterministic headers passed');
    else {
        console.error(error);
        await webFront.cppFunction('test.report')(false, `${error.message ?? error}\n${error.stack ?? ''}`);
    }
} finally {
    if (lastPhase) await webFront.cppFunction('test.progress')(
        `${profileNow().toFixed(3)}ms end ${lastPhase.name} durationMs=${(profileNow() - lastPhase.time).toFixed(3)}`);
    setTimeout(() => globalThis.close(), 0);
}
