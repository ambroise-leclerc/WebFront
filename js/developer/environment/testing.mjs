// wf test: the project's specs run under the Jasmine of its lock, in a dedicated frame of this page. The frame reports
// through postMessage; the report is bounded here because the specs are the project's own code, not trusted output.
import {ScriptExit, lockedPackage} from './scripts.mjs';
import {manifestPath} from './manifest.mjs';

/** Each run publishes its own directory under this one: two sessions on a project never run each other's specs. */
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
const suites = [];
// jasmine.js of version 7 installs the globals itself; earlier versions leave that to their boot file, done here.
function boot() {
    if (globalThis.jasmine) return;
    const jasmine = jasmineRequire.core(jasmineRequire);
    const members = jasmineRequire.interface(jasmine, jasmine.getEnv());
    globalThis.jasmine = jasmine;
    for (const name in members) globalThis[name] = members[name];
}
try {
    try { await webFront.ready; } catch { /* the specs that need the bridge fail on their own */ }
    boot();
    const env = jasmine.getEnv();
    env.configure({random: false, specFilter: spec => spec.getFullName().includes(pattern)});
    env.addReporter({
        specDone(result) {
            specs.push({name: clip(result.fullName, 1024), status: result.status, reason: clip(result.pendingReason, 1024),
                failures: problems(result.failedExpectations)});
            post({type: 'spec', status: result.status});
        },
        suiteDone(result) {
            // An afterAll that throws is reported here, not by any spec.
            if (result.failedExpectations?.length) suites.push({name: clip(result.fullName, 1024), failures: problems(result.failedExpectations)});
        },
        jasmineDone(info) {
            post({type: 'done', version: jasmine.version, specs, suites, overall: info.overallStatus, durationMs: info.totalTime,
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

/** The files of a runner: the page, the module that runs the specs, the locked Jasmine and everything the specs built (JS, CSS, assets). */
export function runnerFiles(compiled, jasmine) {
    const stylesheet = compiled.has('specs.css') ? '<link rel="stylesheet" href="./specs.css">' : '';
    const page = `<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>WebFront tests</title>${stylesheet}` +
        '<script src="/WebFront.js"></script><script src="./jasmine.js"></script>' +
        '<script type="module" src="./runner.js"></script></html>\n';
    return new Map([...compiled, ['index.html', encoder.encode(page)], ['runner.js', encoder.encode(runnerSource)], ['jasmine.js', jasmine]]);
}

const emptyReport = (status, exitCode, extra = {}) => ({status, exitCode, jasmine: '', pattern: '', incomplete: '', total: 0, passed: 0,
    failed: 0, pending: 0, excluded: 0, durationMs: 0, failures: [], pendingSpecs: [], suites: [], global: [], ...extra});
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
    const suites = Array.isArray(data.suites) ? data.suites.slice(0, reportLimits.failures).map(({name, failures}) => ({name: text(name, 1024), messages: problems(failures)})) : [];
    let status = ['passed', 'failed', 'incomplete'].includes(data.overall) ? data.overall : 'failed';
    let incomplete = text(data.incomplete, 1024);
    if (status === 'passed' && (failedSpecs.length || global.length || suites.length)) status = 'failed';
    if (status === 'passed' && !executed) { status = 'incomplete'; incomplete = pattern ? `No spec matches "${pattern}"` : 'No specs found'; }
    return {status, exitCode: status === 'passed' ? 0 : 1, jasmine: text(data.version, 32), pattern, incomplete,
        total: executed, passed: count('passed'), failed: failedSpecs.length, pending: count('pending'), excluded: count('excluded'),
        durationMs: finite(data.durationMs),
        failures: failedSpecs.slice(0, reportLimits.failures).map(({name, failures}) => ({name: text(name, 1024), messages: problems(failures)})),
        pendingSpecs: specs.filter(spec => spec.status === 'pending').slice(0, reportLimits.pending).map(({name, reason}) => ({name: text(name, 1024), reason: text(reason, 1024)})),
        suites, global};
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
    for (const {name, messages} of report.suites) lines.push(`FAIL ${name} (suite)`, ...messages.flatMap(problemLines));
    if (report.global.length) lines.push('Failures outside the specs:', ...report.global.flatMap(problemLines));
    for (const {name, reason} of report.pendingSpecs) lines.push(`PENDING ${name}${reason ? `: ${reason}` : ''}`);
    return lines.join('\n');
}

const progress = {passed: '.', failed: 'F', pending: '*'};
/** The promise, or a rejection with the signal's reason as soon as the signal aborts, whatever the promise is waiting for. */
export function abortable(promise, signal) {
    return new Promise((resolve, reject) => {
        const abort = () => reject(signal.reason);
        if (signal.aborted) abort();
        signal.addEventListener('abort', abort, {once: true});
        promise.then(resolve, reject).finally(() => signal.removeEventListener('abort', abort));
    });
}

export const stoppedRun = (reason, pattern) => {
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
export function runInFrame({pattern, timeout, runId, signal, write = () => {}, createFrame = defaultFrame}) {
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
        const interrupt = () => finish(stoppedRun('interrupted', pattern));
        addEventListener('message', receive);
        signal?.addEventListener('abort', interrupt, {once: true});
        timer = setTimeout(() => finish(stoppedRun('timeout', pattern)), timeout * 1000);
        if (signal?.aborted) return interrupt();
        frame.src = `/webfront/developer/test/${runId}/index.html?pattern=${encodeURIComponent(pattern)}&run=${Date.now()}`;
        document.body.append(frame);
    });
}

/** The terminal's result of a report: its text on success, a ScriptExit carrying the report otherwise. */
export function reportResult(report) {
    const output = formatReport(report);
    if (report.exitCode === 0) return output;
    throw Object.assign(new ScriptExit(output, report.exitCode), {report});
}

// A bridge string carries a 16-bit length, in UTF-8 bytes: the result must fit in one, with a margin.
export const resultLimit = 60000;
const byteLength = value => encoder.encode(JSON.stringify(value)).length;
const clipText = (text, length) => text.length > length ? `${text.slice(0, length)}…` : text;

/**
 * The result cut to `limit` bytes of JSON, so that it can cross the bridge: the output first, then the stack frames,
 * then the messages, then the lists of failures; the counts and statuses are never changed. A result that was cut says
 * so in `truncated`.
 */
export function fitResult(result, limit = resultLimit) {
    if (byteLength(result) <= limit) return result;
    const fitted = structuredClone(result);
    fitted.truncated = true;
    const problems = report => [...report.failures, ...report.suites ?? []].flatMap(({messages}) => messages).concat(report.global ?? []);
    const steps = [
        ...[4096, 1024, 256].map(length => () => { fitted.output = clipText(fitted.output, length); }),
        () => { for (const problem of problems(fitted.report)) problem.stack = ''; },
        ...[2048, 512, 128].map(length => () => {
            for (const problem of problems(fitted.report)) problem.message = clipText(problem.message, length);
            for (const key of ['error', 'incomplete', 'pattern']) if (typeof fitted.report[key] === 'string') fitted.report[key] = clipText(fitted.report[key], length);
        }),
        ...[10, 3, 0].map(count => () => {
            for (const key of ['failures', 'suites', 'pendingSpecs']) if (fitted.report[key]) fitted.report[key].length = Math.min(fitted.report[key].length, count);
            fitted.report.global.length = Math.min(fitted.report.global.length, count);
        }),
        () => { fitted.output = ''; },
        // Whatever is left: the status and the counts, which are numbers and short words.
        () => {
            const {status, exitCode, jasmine, total, passed, failed, pending, excluded, durationMs} = fitted.report;
            fitted.report = {status, exitCode, jasmine: clipText(String(jasmine ?? ''), 32), total, passed, failed, pending, excluded, durationMs,
                failures: [], pendingSpecs: [], suites: [], global: [], pattern: '', incomplete: '', error: ''};
        },
    ];
    for (const step of fitted.report ? steps : steps.slice(0, 3).concat(steps.at(-2))) {
        step();
        if (byteLength(fitted) <= limit) break;
    }
    return fitted;
}
