// Host tests supplement the real-browser regression of wf test in test/web/developer-specs.mjs.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {build as bundle} from '../vendor/node_modules/esbuild/lib/main.js';
import * as esbuild from '../vendor/node_modules/esbuild/lib/main.js';

// Resolve the browser modules using the same locked dependencies as the environment archive.
async function load(entry) {
    const compiled = await bundle({entryPoints: [entry], bundle: true, write: false, format: 'esm', platform: 'browser',
        nodePaths: ['js/developer/vendor/node_modules']});
    const temporary = await mkdtemp(join(tmpdir(), 'webfront-testing-test-'));
    try {
        const path = join(temporary, 'module.mjs');
        await writeFile(path, compiled.outputFiles[0].contents);
        return await import(pathToFileURL(path));
    } finally { await rm(temporary, {recursive: true, force: true}); }
}
const {parseTestOptions, specFiles, jasmineCore, buildReport, formatReport, reportResult, runnerFiles, fitResult, abortable} = await load('js/developer/environment/testing.mjs');
const {bundleTests} = await load('js/developer/environment/bundler.mjs');
const encode = text => new TextEncoder().encode(text);
const decode = bytes => new TextDecoder().decode(bytes);

test('options: a time limit in whole seconds and one pattern', () => {
    assert.deepEqual(parseTestOptions([]), {timeout: 60, pattern: ''});
    assert.deepEqual(parseTestOptions(['--timeout', '5', 'suite name']), {timeout: 5, pattern: 'suite name'});
    for (const args of [['--timeout'], ['--timeout', '0'], ['--timeout', '1.5'], ['--timeout', '3601']]) assert.throws(() => parseTestOptions(args), /--timeout expects whole seconds from 1 to 3600/);
    for (const args of [['a', 'b'], ['--filter'], ['--timeout', '5', 'a', 'b']]) assert.throws(() => parseTestOptions(args), /Usage: wf test/);
});

test('specs come from the manifest and jasmine-core from the lock, at the version the project chose', () => {
    assert.throws(() => specFiles({}), /No specs/);
    assert.throws(() => specFiles({test: {specs: []}}), /No specs/);
    assert.deepEqual(specFiles({test: {specs: ['a.spec.ts', 'b.spec.js']}}), ['a.spec.ts', 'b.spec.js']);
    const node = (name, version, files) => ({name, version, files, key: `${name}-${version}`, manifest: {}});
    const lock = {format: 1, roots: {'jasmine-core': {id: 'jasmine-core@6.0.0'}}, packages: {
        'jasmine-core@6.0.0': node('jasmine-core', '6.0.0', ['lib/jasmine-core/jasmine.js']),
        'jasmine-core@5.0.0': node('jasmine-core', '5.0.0', ['lib/jasmine-core/jasmine.js'])}};
    assert.equal(jasmineCore(lock).id, 'jasmine-core@6.0.0');
    assert.throws(() => jasmineCore({format: 1, roots: {}, packages: {}}), /run wf install jasmine-core/);
    assert.throws(() => jasmineCore({format: 1, roots: {a: {id: 'jasmine-core@1.0.0'}}, packages: {'jasmine-core@1.0.0': node('jasmine-core', '1.0.0', ['lib/jasmine-core.js'])}}), /no lib\/jasmine-core\/jasmine\.js/);
});

const spec = (name, status, extra = {}) => ({name, status, reason: '', failures: [], ...extra});
const failure = (message, stack = '') => ({message, stack});

test('a report counts exactly, names the failures and fails for anything but a pass', () => {
    const passed = buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 12.4, global: [], specs: [
        spec('a b', 'passed'), spec('a c', 'pending', {reason: 'later'}), spec('d', 'excluded')]});
    assert.deepEqual([passed.status, passed.exitCode, passed.total, passed.passed, passed.pending, passed.excluded, passed.durationMs], ['passed', 0, 2, 1, 1, 1, 12]);
    assert.deepEqual(passed.pendingSpecs, [{name: 'a c', reason: 'later'}]);
    const failed = buildReport({type: 'done', overall: 'failed', durationMs: 3, global: [], specs: [
        spec('x y', 'failed', {failures: [failure('Expected 1 to be 2.', 'Error: Expected 1 to be 2.\n    at <Jasmine>\n    at UserContext.<anonymous> (http://127.0.0.1/specs.js:3:9)')]})]});
    assert.equal(failed.exitCode, 1);
    assert.deepEqual(failed.failures.map(item => item.name), ['x y']);
    // A reporter that says "passed" cannot hide a failed spec or a global failure.
    assert.equal(buildReport({type: 'done', overall: 'passed', global: [], specs: [spec('x', 'failed')]}).status, 'failed');
    assert.equal(buildReport({type: 'done', overall: 'passed', global: [failure('afterAll failed')], specs: [spec('x', 'passed')]}).status, 'failed');
    assert.equal(buildReport({type: 'done', overall: 'bogus', global: [], specs: []}).status, 'failed');
    // Nothing executed is not a pass, with or without a pattern.
    const none = buildReport({type: 'done', overall: 'passed', global: [], specs: [spec('x', 'excluded')]}, {pattern: 'zzz'});
    assert.deepEqual([none.status, none.exitCode, none.incomplete], ['incomplete', 1, 'No spec matches "zzz"']);
    assert.equal(buildReport({type: 'done', overall: 'passed', global: [], specs: []}).incomplete, 'No specs found');
    assert.deepEqual([buildReport({type: 'done', overall: 'incomplete', incomplete: 'fit() or fdescribe() was found', global: [], specs: [spec('x', 'passed')]}).exitCode], [1]);
    const broken = buildReport({type: 'error', message: 'Error: boom'});
    assert.deepEqual([broken.status, broken.exitCode, broken.error], ['error', 1, 'Error: boom']);
});

test('failures of a suite (an afterAll) are collected, counted and shown', () => {
    const report = buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 1, global: [], specs: [spec('cleanup passes', 'passed')],
        suites: [{name: 'cleanup', failures: [failure('afterAll boom')]}]});
    assert.deepEqual([report.status, report.exitCode, report.failed, report.suites.map(suite => suite.name)], ['failed', 1, 0, ['cleanup']]);
    assert.match(formatReport(report), /FAIL cleanup \(suite\)\n  afterAll boom/);
    assert.deepEqual(buildReport({type: 'done', overall: 'passed', global: [], specs: [spec('a', 'passed')]}).suites, []);
});

test('a result is cut to fit a bridge string, keeping the counts and the status', () => {
    const encoded = value => new TextEncoder().encode(JSON.stringify(value)).length;
    const huge = 'é'.repeat(10000);
    const report = buildReport({type: 'done', version: '7.0.2', overall: 'failed', durationMs: 1, global: [failure(huge)], specs:
        Array.from({length: 4}, (_, index) => spec(`big ${index}`, 'failed', {failures: [failure(huge, `Error: ${huge}\n    at f (http://127.0.0.1/specs.js:1:1)`)]}))});
    const result = {exitCode: 1, output: formatReport(report), report};
    assert.ok(encoded(result) > 65535, 'the result of four 10,000-character failures does not fit a bridge string');
    const fitted = fitResult(result);
    assert.ok(encoded(fitted) <= 60000, `fitted to ${encoded(fitted)} bytes`);
    assert.deepEqual([fitted.truncated, fitted.exitCode, fitted.report.status, fitted.report.failed, fitted.report.total], [true, 1, 'failed', 4, 4]);
    assert.deepEqual(fitted.report.failures.map(failure => failure.name), ['big 0', 'big 1', 'big 2', 'big 3']);
    // What fits is left alone, and the original is never changed.
    const small = {exitCode: 0, output: 'ok', report: null};
    assert.equal(fitResult(small), small);
    assert.ok(encoded(result) > 65535 && result.report.failures[0].messages[0].message.length === 10000);
    // A tiny limit still ends with a result that has its counts.
    const tiny = fitResult(result, 2000);
    assert.ok(encoded(tiny) <= 2000 && tiny.report.failed === 4 && tiny.exitCode === 1);
    assert.ok(encoded(fitResult({exitCode: 1, output: 'x'.repeat(100000), report: null})) <= 60000);
    // Every string of the report counts: a load error of NUL characters is six bytes each once encoded as JSON.
    const unloaded = buildReport({type: 'error', message: '\u0000'.repeat(16384)}, {pattern: '\u0000'.repeat(5000)});
    const unloadedResult = {exitCode: 1, output: formatReport(unloaded), report: unloaded};
    assert.ok(encoded(unloadedResult) > 65535);
    const unloadedFitted = fitResult(unloadedResult);
    assert.ok(encoded(unloadedFitted) <= 60000 && unloadedFitted.report.status === 'error' && unloadedFitted.exitCode === 1, `fitted to ${encoded(unloadedFitted)} bytes`);
    // Whatever the report holds, the limit is kept and the status and the counts survive.
    const crowded = buildReport({type: 'done', version: 'v', overall: 'failed', durationMs: 1, specs: Array.from({length: 100}, (_, index) => spec('n'.repeat(1000) + index, index % 2 ? 'failed' : 'pending',
        {reason: 'r'.repeat(1000), failures: [failure('\u0000'.repeat(16000), '\u0001'.repeat(16000))]})), global: [failure('\u0000'.repeat(16000))],
        suites: [{name: 's'.repeat(1000), failures: [failure('\u0000'.repeat(16000))]}], incomplete: 'i'.repeat(1000)}, {pattern: 'p'.repeat(10000)});
    for (const limit of [60000, 20000, 5000]) {
        const result = fitResult({exitCode: 1, output: 'o'.repeat(50000), report: crowded}, limit);
        assert.ok(encoded(result) <= limit && result.report.failed === 50 && result.report.pending === 50 && result.report.status === 'failed', `limit ${limit}: ${encoded(result)} bytes`);
    }
});

test('abortable stops waiting at once, whatever the promise waits for', async () => {
    const controller = new AbortController();
    const never = new Promise(() => {});
    const waiting = abortable(never, controller.signal);
    controller.abort(new Error('stopped'));
    await assert.rejects(waiting, /stopped/);
    await assert.rejects(abortable(never, controller.signal), /stopped/);
    assert.equal(await abortable(Promise.resolve(3), new AbortController().signal), 3);
    await assert.rejects(abortable(Promise.reject(new Error('own')), new AbortController().signal), /own/);
});

test('a report is bounded: what the specs post is cut, not trusted', () => {
    const many = Array.from({length: 300}, (_, index) => spec(`s${index}`, 'failed', {failures: Array.from({length: 20}, () => failure('m'.repeat(50000)))}));
    const report = buildReport({type: 'done', overall: 'failed', specs: many, global: [], durationMs: Infinity, version: 'v'.repeat(100)});
    assert.equal(report.failed, 300);
    assert.equal(report.failures.length, 100);
    assert.equal(report.failures[0].messages.length, 8);
    assert.equal(report.failures[0].messages[0].message.length, 16 * 1024);
    assert.equal(report.durationMs, 0);
    assert.equal(report.jasmine.length, 32);
    assert.equal(buildReport({type: 'done', overall: 'failed', specs: 'not a list', global: 7}).total, 0);
});

test('the terminal text gives the summary, each failure with its frames, then the pending specs', () => {
    const report = buildReport({type: 'done', version: '7.0.2', overall: 'failed', durationMs: 5, global: [failure('afterAll boom')], specs: [
        spec('one', 'passed'), spec('two', 'pending', {reason: 'later'}), spec('three', 'excluded'),
        spec('suite fails', 'failed', {failures: [failure('Expected 1 to be 2.', 'Error: Expected 1 to be 2.\n    at <Jasmine>\n    at UserContext.<anonymous> (http://127.0.0.1/specs.js:3:9)\n    foo@http://127.0.0.1/specs.js:4:5')]})]});
    const lines = formatReport(report).split('\n');
    assert.equal(lines[0], 'FAILED: 3 specs, 1 failure, 1 pending, 1 filtered out (5 ms, jasmine-core 7.0.2)');
    assert.deepEqual(lines.slice(1), ['FAIL suite fails', '  Expected 1 to be 2.', '    at UserContext.<anonymous> (http://127.0.0.1/specs.js:3:9)',
        '    foo@http://127.0.0.1/specs.js:4:5', 'Failures outside the specs:', '  afterAll boom', 'PENDING two: later']);
    assert.match(formatReport(buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 1, global: [], specs: [spec('a', 'passed')]})), /^PASSED: 1 spec, 0 failures, 0 pending \(1 ms/);
    assert.equal(formatReport(broken()), 'The specs did not load: Error: boom');
    function broken() { return buildReport({type: 'error', message: 'Error: boom'}); }
});

test('a result is the text on success and an error with the exit code and the report otherwise', () => {
    const ok = buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 1, global: [], specs: [spec('a', 'passed')]});
    assert.match(reportResult(ok), /^PASSED/);
    const bad = buildReport({type: 'done', version: '7.0.2', overall: 'failed', durationMs: 1, global: [], specs: [spec('a', 'failed')]});
    assert.throws(() => reportResult(bad), error => error.exitCode === 1 && error.report === bad && /^FAILED/.test(error.message));
});

test('the runner page loads the locked jasmine before the specs and the specs as a module', () => {
    const files = runnerFiles(new Map([['specs.js', encode('specs')]]), encode('jasmine'));
    assert.deepEqual([...files.keys()].sort(), ['index.html', 'jasmine.js', 'runner.js', 'specs.js']);
    assert.ok(!decode(files.get('index.html')).includes('stylesheet'));
    // What the specs built is published whole, and the stylesheet is loaded before the specs run.
    const styled = runnerFiles(new Map([['specs.js', encode('s')], ['specs.css', encode('c')], ['assets/logo-1.png', encode('p')]]), encode('jasmine'));
    assert.deepEqual([...styled.keys()].sort(), ['assets/logo-1.png', 'index.html', 'jasmine.js', 'runner.js', 'specs.css', 'specs.js']);
    assert.match(decode(styled.get('index.html')), /<link rel="stylesheet" href="\.\/specs\.css">/);
    // jasmine.js of versions before 7 only exposes jasmineRequire: the runner boots it as boot0.js does.
    assert.match(decode(files.get('runner.js')), /globalThis\.jasmine\) return;[\s\S]*jasmineRequire\.core\(jasmineRequire\)[\s\S]*jasmineRequire\.interface/);
    const page = decode(files.get('index.html'));
    assert.ok(page.indexOf('./jasmine.js') > page.indexOf('/WebFront.js') && page.indexOf('./jasmine.js') < page.indexOf('./runner.js'));
    assert.match(decode(files.get('runner.js')), /specFilter: spec => spec\.getFullName\(\)\.includes\(pattern\)/);
});

test('specs bundle in the manifest order with their imports, TypeScript and locked packages', async () => {
    const files = new Map([['a.spec.ts', "import {value} from './value'; (globalThis as any).order = [...((globalThis as any).order ?? []), 'a' + value];"],
        ['value.ts', "export const value: number = 1;"], ['b.spec.js', "import dep from 'dep'; globalThis.order = [...(globalThis.order ?? []), 'b' + dep];"],
        ['npm/dep-1/index.js', 'export default 2;']]);
    const lock = {format: 1, roots: {dep: {id: 'dep@1.0.0'}}, packages: {'dep@1.0.0': {name: 'dep', version: '1.0.0', key: 'dep-1', files: ['index.js'], manifest: {name: 'dep', version: '1.0.0', main: 'index.js'}, dependencies: {}}}};
    const readProject = async path => {
        const bytes = files.get(path.startsWith('.webfront/developer/packages/') ? `npm/${path.split('/').slice(3).join('/')}` : path);
        if (!bytes) throw new Error(`Cannot read project file: ${path}`);
        return encode(bytes);
    };
    files.set('b.css', '.probe { width: 37px }');
    files.set('b.spec.js', `import './b.css';\n${files.get('b.spec.js')}`);
    const output = await bundleTests(esbuild, {specs: ['a.spec.ts', 'b.spec.js'], lock, readProject});
    assert.deepEqual([...output.keys()].sort(), ['specs.css', 'specs.js']);
    assert.match(decode(output.get('specs.css')), /width: 37px/);
    const code = decode(output.get('specs.js'));
    const evaluated = new Function('globalThis', code.replace(/^export .*$/gm, ''));
    const sandbox = {};
    evaluated(sandbox);
    assert.deepEqual(sandbox.order, ['a1', 'b2'], 'the specs run in the manifest order');
    await assert.rejects(bundleTests(esbuild, {specs: ['absent.spec.js'], lock, readProject}), /absent\.spec\.js/);
});
