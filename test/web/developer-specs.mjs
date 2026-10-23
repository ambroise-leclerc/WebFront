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
        'tools/unawaited.mjs': "export default async context => { context.project.write('generated/script/unawaited.txt', 'done'); };\n",
        'tools/root.mjs': "export default async context => context.console.log('root', JSON.stringify(await context.project.list()));\n",
        'tools/jsx.tsx': "export default async (context: any) => { const element = <b>x</b>; context.console.log('jsx', element.type); };\n",
    };
    for (const [path, text] of Object.entries(scripts)) await writeFile(path, text);
    const mainBefore = await readFile('ui/main.tsx');
    const scriptManifest = {...manifest, permissions: {files: ['tools'], publish: ['generated/script']},
        scripts: Object.fromEntries([...Object.keys(scripts).map(path => [path.split('/')[1].split('.')[0], path]), ['listing', 'wf list'], ['nested', 'wf run loop']])};
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
    const nested = await failure(runtime.execute(['run', '--timeout', '1', 'nested'], runContext()));
    assert(nested?.exitCode === 124, `The time limit must cover a script run through a wf command: ${nested}`);
    assert(/react@19\.3\.1/.test(await runtime.execute(['list'], context)), 'The terminal must stay usable after a termination');
    assert(/completed/.test(await runtime.execute(['run', 'unawaited'], runContext())), 'A script that does not await its write must complete');
    assert(new TextDecoder().decode(await readFile('generated/script/unawaited.txt')) === 'done', 'A write the script did not await must be applied');
    await runtime.execute(['run', 'root'], runContext());
    assert(scriptOutput.includes('root ["generated/","tools/"]\r\n'), `The root must list the entries leading to granted paths: ${scriptOutput.filter(line => line.startsWith('root'))}`);
    await runtime.execute(['run', 'jsx'], runContext());
    assert(scriptOutput.includes('jsx b\r\n'), 'A TSX script must compile its JSX with the automatic runtime');
    // A stop during a large write: the file is either untouched or complete, and no staging is left.
    const writing = new AbortController();
    const big = failure(runtime.execute(['run', 'big'], runContext(writing.signal)));
    await printed('writing');
    writing.abort(new Error('Interrupted'));
    await big;
    const written = await readFile('generated/script/big.bin').then(bytes => bytes.length, () => 0);
    assert(written === 0 || written === 4 * 1024 * 1024, `A stopped write must leave no partial file: ${written} bytes`);
    const staged = JSON.parse(new TextDecoder().decode(await call('list', '.webfront/developer/staging')));
    assert(staged.length === 0, `A stopped write must leave no staged files: ${staged}`);
    // wf exec runs the webfront.bin entry of an installed package, never an npm bin.
    await runtime.execute(['install', 'webfront-fixture-tool', 'webfront-fixture-nodebin'], context);
    assert(/completed/.test(await runtime.execute(['exec', 'webfront-fixture-tool', 'hello'], runContext())), 'wf exec must run a WebFront entry');
    assert(/^webfront-fixture-lib@1\.\d+\.\d+ hello$/.test(new TextDecoder().decode(await readFile('generated/script/tool.txt'))), 'The entry must bundle its locked dependency');
    assert(/only declares an npm "bin"/.test((await failure(runtime.execute(['exec', 'webfront-fixture-nodebin'], runContext())))?.message), 'A package with only an npm bin must be refused');
    assert(/no WebFront executable entry/.test((await failure(runtime.execute(['exec', 'webfront-fixture-lib'], runContext())))?.message), 'A browser package without an entry must be refused');
    // wf check: a real TypeScript type check in a Worker, with the typescript and the declarations of the lock, all
    // served by the local registry.
    const checkRequests = [];
    globalThis.fetch = (input, init) => {
        checkRequests.push(new URL(input instanceof Request ? input.url : input, location.href).href);
        return pageFetch(input, init);
    };
    const checkFiles = {
        'tsconfig.json': JSON.stringify({compilerOptions: {strict: true, target: 'ES2022', module: 'ESNext', moduleResolution: 'Bundler', lib: ['ES2022']}, include: ['check']}),
        'check/main.ts': "import {answer, area} from 'webfront-fixture-typed';\nimport {legacy} from 'webfront-fixture-legacy';\nimport {greet} from 'webfront-fixture-untyped';\nimport {twice} from './util';\n" +
            "export const total: number = twice(answer) + area({width: 2, height: 3});\nexport const word: string = greet(legacy);\n",
        'check/util.ts': 'export const twice = (value: number): number => 2 * value;\n',
    };
    for (const [path, text] of Object.entries(checkFiles)) await writeFile(path, text);
    const checked = async (args = [], extra = {}) => {
        scriptOutput.length = 0;
        const result = await runtime.execute(['check', ...args], {...runContext(), ...extra}).then(text => ({text}), error => ({error}));
        return {...result, output: scriptOutput.join('')};
    };
    try {
        assert(/needs the typescript package: run wf install typescript@6/.test((await checked()).error?.message), 'wf check must ask for typescript');
        await runtime.execute(['install', 'typescript@4.9.5'], context);
        const old = await checked();
        assert(/supports typescript 5\.x and 6\.x .* not typescript@4\.9\.5/.test(old.error?.message), `An unsupported typescript must be refused: ${old.error}`);
        await runtime.execute(['install', 'typescript@5.9.3', 'webfront-fixture-typed', 'webfront-fixture-legacy', 'webfront-fixture-untyped', '@types/webfront-fixture-untyped'], context);
        const five = await checked();
        assert(five.text && five.output.includes('No type errors in 2 project files (TypeScript 5.9.3).'), `TypeScript 5.9 must accept the project: ${five.error}\n${five.output}`);
        await runtime.execute(['install', 'typescript@6.0.3'], context);
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
    const checkStarted = performance.now();
    const longCheck = failure(runtime.execute(['check'], runContext(stopCheck.signal)));
    await printed('Checking 3 root files');
    stopCheck.abort(new Error('Interrupted'));
    const checkStopped = await longCheck;
    assert(checkStopped?.exitCode === 130 && /Type check stopped by Ctrl\+C \(terminated after the 2 s grace period\)/.test(checkStopped.message) &&
        performance.now() - checkStarted < 30000, `Ctrl+C must terminate a long check after the grace period: ${checkStopped}`);
    assert(JSON.stringify([await listing(''), await listing('check')]) === JSON.stringify(before), 'A check must not write any project file');
    assert(JSON.parse(await listing('.webfront/developer/staging')).length === 0, 'A check must leave no staged files');
    assert(/typescript@6\.0\.3/.test(await runtime.execute(['list'], context)), 'The terminal must stay usable after a stopped check');
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
    sessionFrame.remove();
    await waitState('second', 'cancelled');
    for (let attempt = 0; await webFront.cppFunction('test.browsers')() !== browsers - 1; ++attempt)
        if (attempt === 200) throw new Error('The connection of a closed browser must be released');
        else await delay(25);
    void secondWait;
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
    await webFront.cppFunction('test.report')(true, 'WASM + npm + TSX + PI + Life animation + deterministic headers + scripts + type check + terminal passed');
} catch (error) {
    console.error(error);
    await webFront.cppFunction('test.report')(false, String(error.stack ?? error));
} finally {
    setTimeout(() => globalThis.close(), 0);
}
