// Autonomous real-engine test: compiled WASM, npm graph, PI bridge, preview, cache and embedding.
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const assert = (condition, message) => { if (!condition) throw new Error(message); };
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
try {
    await webFront.ready;
    for (let attempt = 0; attempt < 300; ++attempt) {
        const text = document.getElementById('status')?.textContent ?? '';
        if (text.startsWith('Bootstrap failed:')) throw new Error(text);
        if (text.startsWith('Developer environment ready.')) break;
        if (attempt === 299) throw new Error('Bootstrap readiness timed out');
        await delay(100);
    }
    const runtime = await import('/webfront/developer/environment/runtime.mjs');
    const config = JSON.parse(await webFront.cppFunction('webfront.developer.config')());
    const environment = await (await fetch('/webfront/developer/environment/environment.json')).json();
    config.starters = Object.fromEntries(Object.entries(environment.starters).map(([name, value]) => [name, {url: new URL(value.archive, new URL(config.environment, location.href)).href, integrity: value.integrity}]));
    const context = {bridge: webFront, config, preview: document.getElementById('preview'), signal: new AbortController().signal,
        write: text => console.log(text)};
    await runtime.execute(['create', 'pi'], context);
    const overwriteRefused = await runtime.execute(['create', 'pi'], context).then(() => false, () => true);
    assert(overwriteRefused, 'A starter must not replace an existing project');
    const noManifest = await runtime.execute(['install', 'react@19.3.1'], context).then(() => false, error => /run wf init/.test(error.message));
    assert(noManifest, 'wf install must ask for a manifest first');
    await runtime.execute(['init'], context);
    const initRefused = await runtime.execute(['init'], context).then(() => false, () => true);
    assert(initRefused, 'wf init must not replace an existing manifest');
    await runtime.execute(['install', 'react@19.3.1', 'react-dom@19.3.1'], context);
    const lock = await runtime.readLock(webFront);
    assert(lock.manifestDigest?.length === 64, 'The lock must record the digest of the manifest dependencies');
    const manifest = JSON.parse(new TextDecoder().decode(await readFile('webfront.project.json')));
    assert(manifest.dependencies.react === '19.3.1' && manifest.dependencies['react-dom'] === '19.3.1', 'The manifest must record the requested ranges');
    // Entries and providers declared in the manifest drive wf preview and wf embed.
    manifest.entries = {main: 'ui/main.tsx'};
    manifest.providers = {PiUI: {entry: 'main', output: 'generated/PiUI.hpp'}};
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    assert(!(await runtime.execute(['list'], context)).includes('Warning'), 'Editing entries must not desynchronize the lock');
    assert(Object.keys(lock.packages).length === 3, 'React, React DOM and scheduler must be resolved');
    const listed = await runtime.execute(['list'], context);
    assert(listed.split('\n').length === 3 && listed.includes('react@19.3.1'), 'wf list must name the installed packages');
    const historyLength = history.length;
    await runtime.execute(['preview'], context);
    const frame = context.preview;
    for (let attempt = 0; attempt < 300; ++attempt) {
        if (frame.contentDocument?.querySelector('button')) break;
        if (attempt === 299) throw new Error('React preview did not mount');
        await delay(50);
    }
    frame.contentDocument.querySelector('button').click();
    for (let attempt = 0; attempt < 100; ++attempt) {
        if (frame.contentDocument.querySelector('output')?.textContent.startsWith('3.14159265358979323846264338327950288419716939937510')) break;
        if (attempt === 99) throw new Error('React preview did not display C++ PI');
        await delay(50);
    }
    await runtime.execute(['embed', 'PiUI'], context);
    // Repeat the browser build to prove reproducibility, including licenses and packed gzip bytes.
    const first = await webFront.cppFunction('test.header')('PiUI');
    await runtime.execute(['embed', 'ui/main.tsx', '--output', 'generated/PiUI.hpp'], context);
    assert(await webFront.cppFunction('test.header')('PiUI') === first, 'Header generation must be deterministic');
    // Keep the original PI regression and exercise the new primary starter in the same engine.
    await runtime.execute(['create', 'life', 'life-ui'], context);
    await runtime.execute(['preview', 'life-ui/main.tsx'], context);
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
    await runtime.execute(['embed', 'life-ui/main.tsx', '--output', 'generated/LifeUI.hpp'], context);
    const lifeHeader = await webFront.cppFunction('test.header')('LifeUI');
    await runtime.execute(['embed', 'life-ui/main.tsx', '--output', 'generated/LifeUI.hpp'], context);
    assert(await webFront.cppFunction('test.header')('LifeUI') === lifeHeader, 'Life header must be deterministic');
    // A dependency added by hand desynchronizes the lock: commands say so and nothing is resolved implicitly.
    manifest.dependencies.scheduler = '*';
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    assert((await runtime.execute(['list'], context)).startsWith('Warning'), 'A desynchronized lock must be reported');
    const refused = await runtime.execute(['install'], context).then(() => false, error => /wf install --update/.test(error.message));
    assert(refused, 'wf install must not resolve a desynchronized manifest');
    delete manifest.dependencies.scheduler;
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    assert(/Installed 3 packages/.test(await runtime.execute(['install'], context)), 'A consistent lock must install its exact graph');
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
        const missing = await runtime.execute(['restore', '--offline'], context).then(() => '', error => error.message);
        assert(/react@19\.3\.1 \(missing\)/.test(missing) && requests.length === 0, 'An offline check must list missing packages without any request');
        await writeFile('webfront.project.json', JSON.stringify({...manifest, registry: 'https://registry.invalid/'}, null, 2));
        const restored = await runtime.execute(['restore'], context);
        assert(/Restored 3 of 3 packages/.test(restored) && restored.includes('Registry: /fixtures/registry/ (application option)'), `wf restore must use and name the application registry: ${restored}`);
        assert(requests.length > 0 && requests.every(url => url.startsWith(`${location.origin}/fixtures/registry/`)), 'wf restore must contact the application registry only');
        requests.length = 0;
        assert(/Cache verified offline: 3 packages/.test(await runtime.execute(['restore', '--offline'], context)) && requests.length === 0, 'A complete cache must verify offline');
    } finally {
        globalThis.fetch = pageFetch;
    }
    await writeFile('webfront.project.json', JSON.stringify(manifest, null, 2));
    const previous = frame.contentWindow.location.href;
    await runtime.execute(['preview'], context);
    const piMounted = () => frame.contentDocument?.querySelector('output') && !frame.contentDocument.querySelector('[data-testid]');
    for (let attempt = 0; frame.contentWindow.location.href === previous || !piMounted(); ++attempt) {
        if (attempt === 300) throw new Error('The preview did not build from the restored cache');
        await delay(50);
    }
    const absent = await runtime.execute(['install', 'webfront-fixture-absent'], context).then(() => '', error => error.message);
    assert(/HTTP 404/.test(absent), 'A package absent from the registry must reject');
    const failed = await runtime.execute(['install', 'INVALID-NPM-NAME'], context).then(() => false, () => true);
    assert(failed, 'Missing packages must reject');
    assert(JSON.stringify(await runtime.readLock(webFront)) === JSON.stringify(lock), 'A failed install must preserve the lock');
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
    };
    for (const [path, text] of Object.entries(scripts)) await writeFile(path, text);
    const mainBefore = await readFile('ui/main.tsx');
    const scriptManifest = {...manifest, permissions: {files: ['tools'], publish: ['generated/script']},
        scripts: Object.fromEntries([...Object.keys(scripts).map(path => [path.split('/')[1].split('.')[0], path]), ['listing', 'wf list']])};
    await writeFile('webfront.project.json', JSON.stringify(scriptManifest, null, 2));
    assert(/Script gen completed/.test(await runtime.execute(['run', 'gen', 'a', 'b'], runContext())), 'A TypeScript script must run');
    assert(new TextDecoder().decode(await readFile('generated/script/out.txt')) === 'a+b' && scriptOutput.includes('wrote a+b\r\n'), 'The script must write an allowed file and stream its console');
    assert(/react@19\.3\.1/.test(await runtime.execute(['run', 'listing'], runContext())), 'A script may run a wf command');
    const denied = await failure(runtime.execute(['run', 'denied'], runContext()));
    assert(/Permission denied: writing ui\/main\.tsx/.test(denied?.message) && denied.exitCode === 1, `A context operation outside the permissions must be refused: ${denied}`);
    assert(new TextDecoder().decode(await readFile('ui/main.tsx')) === new TextDecoder().decode(mainBefore), 'A refused write must not change the file');
    const node = await failure(runtime.execute(['run', 'node'], runContext()));
    assert(/"fs" is a Node\.js API/.test(node?.message), `A Node.js import must be refused while loading: ${node}`);
    const exited = await failure(runtime.execute(['run', 'fail'], runContext()));
    assert(exited?.exitCode === 3 && /exited with code 3/.test(exited.message), 'A non-zero exit code must be propagated');
    const cooperative = new AbortController();
    const waiting = failure(runtime.execute(['run', 'coop'], runContext(cooperative.signal)));
    await printed('waiting');
    cooperative.abort(new Error('Interrupted'));
    const stopped = await waiting;
    assert(stopped?.exitCode === 130 && !/grace period/.test(stopped.message), `Ctrl+C must stop a cooperative script: ${stopped}`);
    const interrupt = new AbortController();
    const started = performance.now();
    const looping = failure(runtime.execute(['run', 'loop'], runContext(interrupt.signal)));
    await printed('looping');
    interrupt.abort(new Error('Interrupted'));
    const killed = await looping;
    assert(killed?.exitCode === 130 && /terminated after the 2 s grace period/.test(killed.message) && performance.now() - started < 15000, `A busy script must be terminated after Ctrl+C: ${killed}`);
    const timedOut = await failure(runtime.execute(['run', '--timeout', '1', 'loop'], runContext()));
    assert(timedOut?.exitCode === 124 && /stopped at its time limit/.test(timedOut.message), `The time limit must stop a busy script: ${timedOut}`);
    assert(/react@19\.3\.1/.test(await runtime.execute(['list'], context)), 'The terminal must stay usable after a termination');
    // A stop during a large write: the file is either untouched or complete, and no staging is left.
    const writing = new AbortController();
    const big = failure(runtime.execute(['run', 'big'], runContext(writing.signal)));
    await printed('writing');
    writing.abort(new Error('Interrupted'));
    await big;
    const written = await readFile('generated/script/big.bin').then(bytes => bytes.length, () => 0);
    assert(written === 0 || written === 4 * 1024 * 1024, `A stopped write must leave no partial file: ${written} bytes`);
    assert((await call('list', '.webfront/developer/staging')).length === 0, 'A stopped write must leave no staged files');
    // wf exec runs the webfront.bin entry of an installed package, never an npm bin.
    await runtime.execute(['install', 'webfront-fixture-tool', 'webfront-fixture-nodebin'], context);
    assert(/completed/.test(await runtime.execute(['exec', 'webfront-fixture-tool', 'hello'], runContext())), 'wf exec must run a WebFront entry');
    assert(/^webfront-fixture-lib@1\.\d+\.\d+ hello$/.test(new TextDecoder().decode(await readFile('generated/script/tool.txt'))), 'The entry must bundle its locked dependency');
    assert(/only declares an npm "bin"/.test((await failure(runtime.execute(['exec', 'webfront-fixture-nodebin'], runContext())))?.message), 'A package with only an npm bin must be refused');
    assert(/no WebFront executable entry/.test((await failure(runtime.execute(['exec', 'webfront-fixture-lib'], runContext())))?.message), 'A browser package without an entry must be refused');
    await webFront.cppFunction('test.report')(true, 'WASM + npm + TSX + PI + Life animation + deterministic headers + scripts passed');
} catch (error) {
    console.error(error);
    await webFront.cppFunction('test.report')(false, String(error.stack ?? error));
} finally {
    setTimeout(() => globalThis.close(), 0);
}
