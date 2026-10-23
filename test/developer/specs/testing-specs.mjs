// Specs of wf test's host side, which supplement the real-browser regression in test/web/developer-specs.mjs.
import {parseTestOptions, specFiles, jasmineCore, buildReport, formatReport, reportResult, runnerFiles, fitResult, abortable} from '../../../js/developer/environment/testing.mjs';
import {rejects, throws} from '../support.mjs';

describe('wf test', () => {
    const encode = text => new TextEncoder().encode(text);
    const decode = bytes => new TextDecoder().decode(bytes);

    it('options: a time limit in whole seconds and one pattern', () => {
        expect(parseTestOptions([])).toEqual({timeout: 60, pattern: ''});
        expect(parseTestOptions(['--timeout', '5', 'suite name'])).toEqual({timeout: 5, pattern: 'suite name'});
        for (const args of [['--timeout'], ['--timeout', '0'], ['--timeout', '1.5'], ['--timeout', '3601']]) throws(() => parseTestOptions(args), /--timeout expects whole seconds from 1 to 3600/);
        for (const args of [['a', 'b'], ['--filter'], ['--timeout', '5', 'a', 'b']]) throws(() => parseTestOptions(args), /Usage: wf test/);
    });

    it('specs come from the manifest and jasmine-core from the lock, at the version the project chose', () => {
        throws(() => specFiles({}), /No specs/);
        throws(() => specFiles({test: {specs: []}}), /No specs/);
        expect(specFiles({test: {specs: ['a.spec.ts', 'b.spec.js']}})).toEqual(['a.spec.ts', 'b.spec.js']);
        const node = (name, version, files) => ({name, version, files, key: `${name}-${version}`, manifest: {}});
        const lock = {format: 1, roots: {'jasmine-core': {id: 'jasmine-core@6.0.0'}}, packages: {
            'jasmine-core@6.0.0': node('jasmine-core', '6.0.0', ['lib/jasmine-core/jasmine.js']),
            'jasmine-core@5.0.0': node('jasmine-core', '5.0.0', ['lib/jasmine-core/jasmine.js'])}};
        expect(jasmineCore(lock).id).toBe('jasmine-core@6.0.0');
        throws(() => jasmineCore({format: 1, roots: {}, packages: {}}), /run wf install jasmine-core/);
        throws(() => jasmineCore({format: 1, roots: {a: {id: 'jasmine-core@1.0.0'}}, packages: {'jasmine-core@1.0.0': node('jasmine-core', '1.0.0', ['lib/jasmine-core.js'])}}), /no lib\/jasmine-core\/jasmine\.js/);
    });

    const spec = (name, status, extra = {}) => ({name, status, reason: '', failures: [], ...extra});
    const failure = (message, stack = '') => ({message, stack});

    it('a report counts exactly, names the failures and fails for anything but a pass', () => {
        const passed = buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 12.4, global: [], specs: [
            spec('a b', 'passed'), spec('a c', 'pending', {reason: 'later'}), spec('d', 'excluded')]});
        expect([passed.status, passed.exitCode, passed.total, passed.passed, passed.pending, passed.excluded, passed.durationMs]).toEqual(['passed', 0, 2, 1, 1, 1, 12]);
        expect(passed.pendingSpecs).toEqual([{name: 'a c', reason: 'later'}]);
        const failed = buildReport({type: 'done', overall: 'failed', durationMs: 3, global: [], specs: [
            spec('x y', 'failed', {failures: [failure('Expected 1 to be 2.', 'Error: Expected 1 to be 2.\n    at <Jasmine>\n    at UserContext.<anonymous> (http://127.0.0.1/specs.js:3:9)')]})]});
        expect(failed.exitCode).toBe(1);
        expect(failed.failures.map(item => item.name)).toEqual(['x y']);
        // A reporter that says "passed" cannot hide a failed spec or a global failure.
        expect(buildReport({type: 'done', overall: 'passed', global: [], specs: [spec('x', 'failed')]}).status).toBe('failed');
        expect(buildReport({type: 'done', overall: 'passed', global: [failure('afterAll failed')], specs: [spec('x', 'passed')]}).status).toBe('failed');
        expect(buildReport({type: 'done', overall: 'bogus', global: [], specs: []}).status).toBe('failed');
        // Nothing executed is not a pass, with or without a pattern.
        const none = buildReport({type: 'done', overall: 'passed', global: [], specs: [spec('x', 'excluded')]}, {pattern: 'zzz'});
        expect([none.status, none.exitCode, none.incomplete]).toEqual(['incomplete', 1, 'No spec matches "zzz"']);
        expect(buildReport({type: 'done', overall: 'passed', global: [], specs: []}).incomplete).toBe('No specs found');
        expect([buildReport({type: 'done', overall: 'incomplete', incomplete: 'fit() or fdescribe() was found', global: [], specs: [spec('x', 'passed')]}).exitCode]).toEqual([1]);
        const broken = buildReport({type: 'error', message: 'Error: boom'});
        expect([broken.status, broken.exitCode, broken.error]).toEqual(['error', 1, 'Error: boom']);
    });

    it('failures of a suite (an afterAll) are collected, counted and shown', () => {
        const report = buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 1, global: [], specs: [spec('cleanup passes', 'passed')],
            suites: [{name: 'cleanup', failures: [failure('afterAll boom')]}]});
        expect([report.status, report.exitCode, report.failed, report.suites.map(suite => suite.name)]).toEqual(['failed', 1, 0, ['cleanup']]);
        expect(formatReport(report)).toMatch(/FAIL cleanup \(suite\)\n  afterAll boom/);
        expect(buildReport({type: 'done', overall: 'passed', global: [], specs: [spec('a', 'passed')]}).suites).toEqual([]);
    });

    it('a result is cut to fit a bridge string, keeping the counts and the status', () => {
        const encoded = value => new TextEncoder().encode(JSON.stringify(value)).length;
        const huge = 'é'.repeat(10000);
        const report = buildReport({type: 'done', version: '7.0.2', overall: 'failed', durationMs: 1, global: [failure(huge)], specs:
            Array.from({length: 4}, (_, index) => spec(`big ${index}`, 'failed', {failures: [failure(huge, `Error: ${huge}\n    at f (http://127.0.0.1/specs.js:1:1)`)]}))});
        const result = {exitCode: 1, output: formatReport(report), report};
        expect(encoded(result) > 65535).withContext('the result of four 10,000-character failures does not fit a bridge string').toBeTruthy();
        const fitted = fitResult(result);
        expect(encoded(fitted) <= 60000).withContext(`fitted to ${encoded(fitted)} bytes`).toBeTruthy();
        expect([fitted.truncated, fitted.exitCode, fitted.report.status, fitted.report.failed, fitted.report.total]).toEqual([true, 1, 'failed', 4, 4]);
        expect(fitted.report.failures.map(failure => failure.name)).toEqual(['big 0', 'big 1', 'big 2', 'big 3']);
        // What fits is left alone, and the original is never changed.
        const small = {exitCode: 0, output: 'ok', report: null};
        expect(fitResult(small)).toBe(small);
        expect(encoded(result) > 65535 && result.report.failures[0].messages[0].message.length === 10000).toBeTruthy();
        // A tiny limit still ends with a result that has its counts.
        const tiny = fitResult(result, 2000);
        expect(encoded(tiny) <= 2000 && tiny.report.failed === 4 && tiny.exitCode === 1).toBeTruthy();
        expect(encoded(fitResult({exitCode: 1, output: 'x'.repeat(100000), report: null})) <= 60000).toBeTruthy();
        // Every string of the report counts: a load error of NUL characters is six bytes each once encoded as JSON.
        const unloaded = buildReport({type: 'error', message: '\u0000'.repeat(16384)}, {pattern: '\u0000'.repeat(5000)});
        const unloadedResult = {exitCode: 1, output: formatReport(unloaded), report: unloaded};
        expect(encoded(unloadedResult) > 65535).toBeTruthy();
        const unloadedFitted = fitResult(unloadedResult);
        expect(encoded(unloadedFitted) <= 60000 && unloadedFitted.report.status === 'error' && unloadedFitted.exitCode === 1).withContext(`fitted to ${encoded(unloadedFitted)} bytes`).toBeTruthy();
        // Whatever the report holds, the limit is kept and the status and the counts survive.
        const crowded = buildReport({type: 'done', version: 'v', overall: 'failed', durationMs: 1, specs: Array.from({length: 100}, (_, index) => spec('n'.repeat(1000) + index, index % 2 ? 'failed' : 'pending',
            {reason: 'r'.repeat(1000), failures: [failure('\u0000'.repeat(16000), '\u0001'.repeat(16000))]})), global: [failure('\u0000'.repeat(16000))],
            suites: [{name: 's'.repeat(1000), failures: [failure('\u0000'.repeat(16000))]}], incomplete: 'i'.repeat(1000)}, {pattern: 'p'.repeat(10000)});
        for (const limit of [60000, 20000, 5000]) {
            const result = fitResult({exitCode: 1, output: 'o'.repeat(50000), report: crowded}, limit);
            expect(encoded(result) <= limit && result.report.failed === 50 && result.report.pending === 50 && result.report.status === 'failed').withContext(`limit ${limit}: ${encoded(result)} bytes`).toBeTruthy();
        }
    });

    it('abortable stops waiting at once, whatever the promise waits for', async () => {
        const controller = new AbortController();
        const never = new Promise(() => {});
        const waiting = abortable(never, controller.signal);
        controller.abort(new Error('stopped'));
        await rejects(waiting, /stopped/);
        await rejects(abortable(never, controller.signal), /stopped/);
        expect(await abortable(Promise.resolve(3), new AbortController().signal)).toBe(3);
        await rejects(abortable(Promise.reject(new Error('own')), new AbortController().signal), /own/);
    });

    it('a report is bounded: what the specs post is cut, not trusted', () => {
        const many = Array.from({length: 300}, (_, index) => spec(`s${index}`, 'failed', {failures: Array.from({length: 20}, () => failure('m'.repeat(50000)))}));
        const report = buildReport({type: 'done', overall: 'failed', specs: many, global: [], durationMs: Infinity, version: 'v'.repeat(100)});
        expect(report.failed).toBe(300);
        expect(report.failures.length).toBe(100);
        expect(report.failures[0].messages.length).toBe(8);
        expect(report.failures[0].messages[0].message.length).toBe(16 * 1024);
        expect(report.durationMs).toBe(0);
        expect(report.jasmine.length).toBe(32);
        expect(buildReport({type: 'done', overall: 'failed', specs: 'not a list', global: 7}).total).toBe(0);
    });

    it('the terminal text gives the summary, each failure with its frames, then the pending specs', () => {
        const report = buildReport({type: 'done', version: '7.0.2', overall: 'failed', durationMs: 5, global: [failure('afterAll boom')], specs: [
            spec('one', 'passed'), spec('two', 'pending', {reason: 'later'}), spec('three', 'excluded'),
            spec('suite fails', 'failed', {failures: [failure('Expected 1 to be 2.', 'Error: Expected 1 to be 2.\n    at <Jasmine>\n    at UserContext.<anonymous> (http://127.0.0.1/specs.js:3:9)\n    foo@http://127.0.0.1/specs.js:4:5')]})]});
        const lines = formatReport(report).split('\n');
        expect(lines[0]).toBe('FAILED: 3 specs, 1 failure, 1 pending, 1 filtered out (5 ms, jasmine-core 7.0.2)');
        expect(lines.slice(1)).toEqual(['FAIL suite fails', '  Expected 1 to be 2.', '    at UserContext.<anonymous> (http://127.0.0.1/specs.js:3:9)',
            '    foo@http://127.0.0.1/specs.js:4:5', 'Failures outside the specs:', '  afterAll boom', 'PENDING two: later']);
        expect(formatReport(buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 1, global: [], specs: [spec('a', 'passed')]}))).toMatch(/^PASSED: 1 spec, 0 failures, 0 pending \(1 ms/);
        expect(formatReport(broken())).toBe('The specs did not load: Error: boom');
        function broken() { return buildReport({type: 'error', message: 'Error: boom'}); }
    });

    it('a result is the text on success and an error with the exit code and the report otherwise', () => {
        const ok = buildReport({type: 'done', version: '7.0.2', overall: 'passed', durationMs: 1, global: [], specs: [spec('a', 'passed')]});
        expect(reportResult(ok)).toMatch(/^PASSED/);
        const bad = buildReport({type: 'done', version: '7.0.2', overall: 'failed', durationMs: 1, global: [], specs: [spec('a', 'failed')]});
        throws(() => reportResult(bad), error => error.exitCode === 1 && error.report === bad && /^FAILED/.test(error.message));
    });

    it('the runner page loads the locked jasmine before the specs and the specs as a module', () => {
        const files = runnerFiles(new Map([['specs.js', encode('specs')]]), encode('jasmine'));
        expect([...files.keys()].sort()).toEqual(['index.html', 'jasmine.js', 'runner.js', 'specs.js']);
        expect(!decode(files.get('index.html')).includes('stylesheet')).toBeTruthy();
        // What the specs built is published whole, and the stylesheet is loaded before the specs run.
        const styled = runnerFiles(new Map([['specs.js', encode('s')], ['specs.css', encode('c')], ['assets/logo-1.png', encode('p')]]), encode('jasmine'));
        expect([...styled.keys()].sort()).toEqual(['assets/logo-1.png', 'index.html', 'jasmine.js', 'runner.js', 'specs.css', 'specs.js']);
        expect(decode(styled.get('index.html'))).toMatch(/<link rel="stylesheet" href="\.\/specs\.css">/);
        // jasmine.js of versions before 7 only exposes jasmineRequire: the runner boots it as boot0.js does.
        expect(decode(files.get('runner.js'))).toMatch(/globalThis\.jasmine\) return;[\s\S]*jasmineRequire\.core\(jasmineRequire\)[\s\S]*jasmineRequire\.interface/);
        const page = decode(files.get('index.html'));
        expect(page.indexOf('./jasmine.js') > page.indexOf('/WebFront.js') && page.indexOf('./jasmine.js') < page.indexOf('./runner.js')).toBeTruthy();
        expect(decode(files.get('runner.js'))).toMatch(/specFilter: spec => spec\.getFullName\(\)\.includes\(pattern\)/);
    });
});
