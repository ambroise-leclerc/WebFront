// wf test: the project's specs run under the Jasmine of its lock, in a dedicated frame of this page. The frame reports
// through postMessage; the report is bounded here because the specs are the project's own code, not trusted output.
import {ScriptExit, lockedPackage} from './scripts.mjs';
import {manifestPath} from './manifest.mjs';

export const testDirectory = '.webfront/developer/test';
export const testTimeout = {default: 60, bounds: [1, 3600]};
export const reportLimits = {failures: 100, messages: 8, text: 16 * 1024, pending: 100};
const jasminePath = 'lib/jasmine-core/jasmine.js';
const usage = 'wf test [--timeout <seconds>] [pattern]';

/** [--timeout <seconds>] [pattern]: the pattern keeps the specs whose full name (suites and spec) contains it. */
export function parseTestOptions(args) {
    let timeout = testTimeout.default;
    let rest = args;
    if (rest[0] === '--timeout') {
        timeout = Number(rest[1]);
        const [least, most] = testTimeout.bounds;
        if (!Number.isInteger(timeout) || timeout < least || timeout > most)
            throw new Error(`--timeout expects whole seconds from ${least} to ${most}`);
        rest = rest.slice(2);
    }
    if (rest.length > 1 || rest[0]?.startsWith('-')) throw new Error(`Usage: ${usage}`);
    return {timeout, pattern: rest[0] ?? ''};
}

/** The spec files of the manifest's "test.specs", in order. */
export function specFiles(manifest) {
    const specs = manifest?.test?.specs ?? [];
    if (!specs.length) throw new Error(`No specs: declare "test": {"specs": [...]} in ${manifestPath}`);
    return specs;
}

/** The locked jasmine-core, chosen by the project: its id, node and browser file, or an error that says how to install it. */
export function jasmineCore(lock) {
    const locked = lockedPackage(lock, 'jasmine-core');
    if (!locked) throw new Error('jasmine-core is not installed: run wf install jasmine-core');
    const [id, node] = locked;
    if (!node.files.includes(jasminePath)) throw new Error(`${id} has no ${jasminePath}: wf test needs the browser build of jasmine-core`);
    return {id, node, path: jasminePath};
}

const encoder = new TextEncoder();

// The runner is a module: it waits for the bridge when there is one, then runs the specs. A spec that needs C++
// finds webFront ready; one that does not is not held back by a failed connection.
const runnerSource = `const post = message => parent.postMessage({webfrontTest: message}, location.origin);
const pattern = new URLSearchParams(location.search).get('pattern') ?? '';
const clip = (value, limit = ${reportLimits.text}) => String(value ?? '').slice(0, limit);
const problems = list => (list ?? []).slice(0, ${reportLimits.messages}).map(({message, stack}) => ({message: clip(message), stack: clip(stack)}));
const specs = [];
try {
    try { await webFront.ready; } catch { /* the specs that need the bridge fail on their own */ }
    const env = jasmine.getEnv();
    env.configure({random: false, specFilter: spec => spec.getFullName().includes(pattern)});
    env.addReporter({
        specDone(result) {
            specs.push({name: clip(result.fullName, 1024), status: result.status, reason: clip(result.pendingReason, 1024),
                failures: problems(result.failedExpectations)});
            post({type: 'spec', status: result.status});
        },
        jasmineDone(info) {
            post({type: 'done', version: jasmine.version, specs, overall: info.overallStatus, durationMs: info.totalTime,
                incomplete: clip(info.incompleteReason, 1024), global: problems(info.failedExpectations)});
        },
    });
    await import('./specs.js');
    await env.execute();
} catch (error) {
    // WebKit's stack lacks the message, so it is always put first.
    post({type: 'error', message: clip(error?.message ? error.name + ': ' + error.message + '\\n' + (error.stack ?? '') : error?.stack ?? error)});
}
`;

/** The files of a runner: the page, the module that runs the specs, the locked Jasmine and the compiled specs. */
export function runnerFiles(specs, jasmine) {
    const page = '<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>WebFront tests</title>' +
        '<script src="/WebFront.js"></script><script src="./jasmine.js"></script>' +
        '<script type="module" src="./runner.js"></script></html>\n';
    return new Map([['index.html', encoder.encode(page)], ['runner.js', encoder.encode(runnerSource)],
        ['jasmine.js', jasmine], ['specs.js', specs]]);
}

const emptyReport = (status, exitCode, extra = {}) => ({status, exitCode, jasmine: '', pattern: '', incomplete: '', total: 0, passed: 0,
    failed: 0, pending: 0, excluded: 0, durationMs: 0, failures: [], pendingSpecs: [], global: [], ...extra});
const finite = value => Number.isFinite(value) && value >= 0 ? Math.round(value) : 0;
const text = (value, limit) => typeof value === 'string' ? value.slice(0, limit) : '';
const problems = list => Array.isArray(list) ? list.slice(0, reportLimits.messages).map(item =>
    ({message: text(item?.message, reportLimits.text), stack: text(item?.stack, reportLimits.text)})) : [];

/**
 * The report of a finished run, from what the frame posted. Counts are exact; the lists of failures and of pending
 * specs are cut at their limits.
 *   status: passed | failed | incomplete (no spec, or fit/fdescribe) | error (the specs did not load);
 *   exitCode: 0 only for passed.
 */
export function buildReport(data, {pattern = ''} = {}) {
    if (data.type === 'error') return emptyReport('error', 1, {pattern, error: text(data.message, reportLimits.text)});
    const specs = Array.isArray(data.specs) ? data.specs : [];
    const count = status => specs.filter(spec => spec.status === status).length;
    const failedSpecs = specs.filter(spec => spec.status === 'failed');
    const executed = specs.filter(spec => spec.status !== 'excluded').length;
    const global = problems(data.global);
    let status = ['passed', 'failed', 'incomplete'].includes(data.overall) ? data.overall : 'failed';
    let incomplete = text(data.incomplete, 1024);
    if (status === 'passed' && (failedSpecs.length || global.length)) status = 'failed';
    if (status === 'passed' && !executed) { status = 'incomplete'; incomplete = pattern ? `No spec matches "${pattern}"` : 'No specs found'; }
    return {status, exitCode: status === 'passed' ? 0 : 1, jasmine: text(data.version, 32), pattern, incomplete,
        total: executed, passed: count('passed'), failed: failedSpecs.length, pending: count('pending'), excluded: count('excluded'),
        durationMs: finite(data.durationMs),
        failures: failedSpecs.slice(0, reportLimits.failures).map(({name, failures}) => ({name: text(name, 1024), messages: problems(failures)})),
        pendingSpecs: specs.filter(spec => spec.status === 'pending').slice(0, reportLimits.pending).map(({name, reason}) => ({name: text(name, 1024), reason: text(reason, 1024)})),
        global};
}

const plural = (count, word) => `${count} ${word}${count === 1 ? '' : 's'}`;
const trace = stack => stack.split('\n').filter(line => /(^|\s|@)\S+:\d+(:\d+)?\)?$/.test(line.trim())).slice(0, 3).map(line => `    ${line.trim()}`);
const problemLines = ({message, stack}) => [`  ${message.split('\n').join('\n  ')}`, ...trace(stack)];

/** The terminal's text for a report: a summary line, then each failure, global failure and pending spec. */
export function formatReport(report) {
    if (report.status === 'error') return `The specs did not load: ${report.error}`;
    const summary = [plural(report.total, 'spec'), plural(report.failed, 'failure'), `${report.pending} pending`];
    if (report.excluded) summary.push(`${report.excluded} filtered out`);
    const lines = [`${report.status === 'passed' ? 'PASSED' : 'FAILED'}: ${summary.join(', ')} (${report.durationMs} ms, jasmine-core ${report.jasmine})`];
    if (report.incomplete) lines.push(report.incomplete);
    for (const {name, messages} of report.failures) lines.push(`FAIL ${name}`, ...messages.flatMap(problemLines));
    if (report.global.length) lines.push('Failures outside the specs:', ...report.global.flatMap(problemLines));
    for (const {name, reason} of report.pendingSpecs) lines.push(`PENDING ${name}${reason ? `: ${reason}` : ''}`);
    return lines.join('\n');
}

const progress = {passed: '.', failed: 'F', pending: '*'};
const stopped = (reason, pattern) => {
    const [message, exitCode] = reason === 'timeout' ? ['Tests stopped at their time limit', 124] : ['Tests stopped by Ctrl+C', 130];
    return Object.assign(new ScriptExit(message, exitCode), {report: emptyReport(reason, exitCode, {pattern, error: message})});
};

// Out of sight but laid out like a page: specs that measure the DOM need a real viewport.
function defaultFrame() {
    const frame = document.createElement('iframe');
    frame.title = 'WebFront tests';
    frame.setAttribute('aria-hidden', 'true');
    frame.style.cssText = 'position:fixed;left:-10000px;top:0;width:1024px;height:768px;border:0';
    return frame;
}

/**
 * Runs the published runner in a frame of its own and resolves with its report. It rejects with a ScriptExit when
 * `signal` aborts (130) or after `timeout` seconds (124); the frame is removed in every case. `createFrame` makes the
 * frame element.
 */
export function runInFrame({pattern, timeout, signal, write = () => {}, createFrame = defaultFrame}) {
    return new Promise((resolve, reject) => {
        const frame = createFrame();
        let timer;
        const finish = (error, report) => {
            clearTimeout(timer);
            removeEventListener('message', receive);
            signal?.removeEventListener('abort', interrupt);
            frame.remove();
            error ? reject(error) : resolve(report);
        };
        const receive = ({source, origin, data}) => {
            if (source !== frame.contentWindow || origin !== location.origin) return;
            const message = data?.webfrontTest;
            if (message?.type === 'spec') write(progress[message.status] ?? '');
            else if (message?.type === 'done' || message?.type === 'error') finish(null, buildReport(message, {pattern}));
        };
        const interrupt = () => finish(stopped('interrupted', pattern));
        addEventListener('message', receive);
        signal?.addEventListener('abort', interrupt, {once: true});
        timer = setTimeout(() => finish(stopped('timeout', pattern)), timeout * 1000);
        if (signal?.aborted) return interrupt();
        frame.src = `/webfront/developer/test/index.html?pattern=${encodeURIComponent(pattern)}&run=${Date.now()}`;
        document.body.append(frame);
    });
}

/** The terminal's result of a report: its text on success, a ScriptExit carrying the report otherwise. */
export function reportResult(report) {
    const output = formatReport(report);
    if (report.exitCode === 0) return output;
    throw Object.assign(new ScriptExit(output, report.exitCode), {report});
}
