// The panel of the bridge in the page (#684, ADR-027 section 8): whether webFront.ready settled and why it was rejected,
// the state of the connection, the calls to C++ that are pending and for how long, the Promises rejected lately, and the
// JavaScript functions that C++ called. That state lives in the page of the application. The page that holds the tab follows
// it with a monitor, which installs a probe in the bridge only while an inspector reads. Attached, the panel is a frame of
// that page and reads the monitor directly; detached, it is another window that does not see the page, so the page publishes
// the same rows to C++ (webfront.inspector.publishBridge) and the panel reads them back (webfront.inspector.bridge).
import {createPoller, createTable, createToolbar, filterRows, formatDuration, readFailure} from './inspector-registry.mjs';

export const sourceFunction = 'webfront.inspector.bridgeSource';
export const publishFunction = 'webfront.inspector.publishBridge';
export const bridgeFunction = 'webfront.inspector.bridge';
/** The JavaScript function that C++ calls to ask this page to publish: `wfInspect.publish`. */
export const publisherGlobal = 'wfInspect';

/**
 * The bounds of a state: the oldest pending calls, the latest rejections and the most called JavaScript functions shown,
 * and the bytes of a text. Together they stay under the 256 rows that C++ keeps for a page.
 */
export const limits = Object.freeze({pending: 100, rejections: 50, jsFunctions: 100, jsNames: 1000, text: 256});
/**
 * A monitor keeps its probe in the bridge this long after its last read, in milliseconds: longer than the slowest period of
 * a panel (5 s), so that a panel that reads never leaves a moment without a probe.
 */
export const defaultLease = 15000;
/** A page publishes at this period while a detached panel reads, in milliseconds. */
export const publishPeriod = 250;

// --- Texts -----------------------------------------------------------------------------------------------------

const encoder = new TextEncoder();
const controls = /[\u0000-\u001f\u007f]/g;

/**
 * A text of the page made safe to show and bounded, as C++ bounds it (inspect::boundedText): control characters become
 * their visible pictures, then the text is cut to `limit` UTF-8 bytes on a character boundary, ending with "…" when cut.
 */
export function boundedText(value, limit = limits.text) {
    let text;
    try { text = String(value); }
    catch { text = '(a value that cannot be read)'; }
    text = text.replace(controls, control => String.fromCodePoint(control === '\u007f' ? 0x2421 : 0x2400 + control.codePointAt(0)));
    if (encoder.encode(text).length <= limit) return text;
    let cut = '', bytes = 3;  // the ellipsis takes three bytes
    for (const character of text) {
        const size = encoder.encode(character).length;
        if (bytes + size > limit) break;
        cut += character;
        bytes += size;
    }
    return `${cut}…`;
}

/** Why a Promise was rejected, as a text: the code of an error that has one, then its message. */
export function rejectionReason(error) {
    try {
        // By its `message`, not by its constructor: an error of the interface's frame is not an Error of this page.
        const described = error !== null && typeof error === 'object';
        const message = described && typeof error.message === 'string' ? error.message : String(error);
        const code = described && typeof error.code === 'string' ? error.code : '';
        return boundedText(code && code !== 'webfront.error' ? `${code}: ${message}` : message);
    } catch { return 'A rejection whose reason cannot be read'; }
}

/** The inspectors' own traffic, which a monitor does not show: reading the state would otherwise change it. */
export const ownTraffic = name => typeof name === 'string' && (name.startsWith('webfront.inspector.') || name.startsWith(`${publisherGlobal}.`));

// --- Monitor ---------------------------------------------------------------------------------------------------

/**
 * Follows the bridge of a page. `target()` gives what to follow now: `{bridge, label, path}`, or null when the page has no
 * bridge; it is asked at each read, since the interface's frame is loaded again by each build. `read()` gives the state as
 * an object (see `emptyState`) and keeps the probe in the bridge for `lease` more milliseconds: without a read the probe is
 * removed, and the bridge records nothing. Calls that were pending when the probe came are shown without a duration.
 * `clock()` is a monotonic time in milliseconds, `now()` the time of day, `timers` gives setTimeout and clearTimeout.
 */
export function createBridgeMonitor({target, clock = () => performance.now(), now = () => Date.now(), timers = globalThis, lease = defaultLease}) {
    let followed = null, sequence = 0, until = 0, timer = null;
    // The state of each bridge it followed, kept while that bridge lives: the target goes back to the page's own bridge
    // each time the interface's frame is loaded again, and a call that is already followed must not be wrapped again.
    const states = new WeakMap();

    function follow(bridge) {
        const known = states.get(bridge);
        if (known) return known;
        const state = {bridge, since: now(), pending: new Set(), tracked: new WeakSet(), rejections: [], rejected: 0, js: new Map(), jsOthers: 0};
        const settle = (entry, failed, error) => {
            if (!state.pending.delete(entry)) return;
            if (!failed) return;
            ++state.rejected;
            state.rejections.push({id: ++sequence, name: entry.name, reason: rejectionReason(error), at: now()});
            if (state.rejections.length > limits.rejections) state.rejections.shift();
        };
        const track = (name, started, resolve, reject) => {
            const entry = {id: ++sequence, name: boundedText(name), started};
            state.pending.add(entry);
            const settlers = [value => { settle(entry, false); resolve(value); }, error => { settle(entry, true, error); reject(error); }];
            state.tracked.add(settlers[0]);
            return settlers;
        };
        // Nothing here may throw: a probe that fails must never fail a call of the application.
        state.probe = {
            call(name, resolve, reject) {
                try { return ownTraffic(name) ? [resolve, reject] : track(name, clock(), resolve, reject); }
                catch { return [resolve, reject]; }
            },
            jsCall(name) {
                try {
                    if (ownTraffic(name)) return;
                    // A long name is kept by its bounded label: what the monitor keeps of a name never exceeds the bound.
                    const key = typeof name === 'string' && name.length <= limits.text / 4 ? name : boundedText(name);
                    const known = state.js.get(key);
                    if (known) ++known.calls;
                    else if (state.js.size < limits.jsNames) state.js.set(key, {id: ++sequence, name: boundedText(key), calls: 1});
                    else ++state.jsOthers;
                } catch { /* never the application's problem */ }
            },
        };
        // The calls made while no probe was there (before the first read, or between two leases): their name is known, not
        // when they started. A call that is already followed is left as it is.
        state.adopt = () => {
            try {
                for (const call of bridge.pendingCalls?.values() ?? []) {
                    if (typeof call?.resolve !== 'function' || typeof call?.reject !== 'function' || state.tracked.has(call.resolve)
                        || ownTraffic(call.name)) continue;
                    [call.resolve, call.reject] = track(call.name ?? '(unknown)', null, call.resolve, call.reject);
                }
            } catch { /* a bridge of another version: only the calls made from now on are followed */ }
        };
        states.set(bridge, state);
        return state;
    }

    function install() {
        if (!followed || followed.bridge.probe === followed.probe) return;
        followed.adopt();
        followed.bridge.probe = followed.probe;
    }
    function release() {
        timers.clearTimeout(timer);
        timer = null;
        try { if (followed && followed.bridge.probe === followed.probe) followed.bridge.probe = null; }
        catch { /* the frame of the bridge is gone */ }
        // No strong reference to a bridge that is not observed: the frame of an interface that was loaded again can go. Its
        // state stays in `states` for as long as that bridge lives, and the calls already followed settle through it.
        followed = null;
    }
    function renew() {
        until = clock() + lease;
        if (timer !== null) return;
        const check = () => {
            timer = null;
            const left = until - clock();
            if (left <= 0) release();
            else timer = timers.setTimeout(check, left);
        };
        timer = timers.setTimeout(check, lease);
    }

    return {
        /** Whether the probe is in the bridge now. */
        get observing() { return Boolean(followed) && followed.bridge.probe === followed.probe; },
        read() {
            let current = null;
            try { current = target(); }
            catch { /* a frame of another origin, or one that is going away */ }
            if (!current?.bridge) {
                release();
                followed = null;
                return emptyState();
            }
            if (followed?.bridge !== current.bridge) {
                release();
                followed = follow(current.bridge);
            }
            try { install(); }
            catch { return emptyState(); }
            renew();
            const {bridge} = followed, time = clock();
            const pending = [...followed.pending].slice(0, limits.pending)
                .map(({id, name, started}) => ({id, name, elapsed: started === null ? null : Math.max(0, time - started)}));
            const jsFunctions = [...followed.js.values()].sort((a, b) => b.calls - a.calls || a.id - b.id).slice(0, limits.jsFunctions)
                .map(entry => ({...entry}));
            const settled = Boolean(bridge.readySettled);
            return {
                page: {label: boundedText(current.label ?? ''), path: boundedText(current.path ?? '')},
                ready: {state: !settled ? 'pending' : bridge.readyError ? 'rejected' : 'resolved',
                    reason: settled && bridge.readyError ? rejectionReason(bridge.readyError) : ''},
                connection: boundedText(bridge.state ?? 'unknown', 16),
                since: followed.since,
                pendingTotal: followed.pending.size,
                rejectedTotal: followed.rejected,
                jsTotal: followed.js.size + (followed.jsOthers ? 1 : 0),
                // Past the names it keeps, calls are counted without their name: how many other functions is not known.
                jsSaturated: followed.jsOthers > 0,
                pending,
                rejections: followed.rejections.map(entry => ({...entry})),
                jsFunctions,
            };
        },
        /** Removes the probe now, as the end of the lease would. */
        release,
    };
}

/**
 * What to follow for a page that shows the interface in `frame`: the bridge of the interface when its page has one, since
 * that is where the application calls C++; null otherwise (no interface yet, or a page without WebFront.js), which leaves
 * the bridge of the page that holds the frame.
 */
export function interfaceTarget(frame) {
    const shown = frame?.contentWindow;
    const bridge = shown?.webFront;  // throws for a frame of another origin: the monitor then shows no bridge
    return bridge ? {bridge, label: 'interface', path: shown.location.pathname} : null;
}

/** The state of a page without a bridge. */
export const emptyState = () => ({page: {label: '', path: ''}, ready: {state: 'none', reason: ''}, connection: 'none', since: 0,
    pendingTotal: 0, rejectedTotal: 0, jsTotal: 0, jsSaturated: false, pending: [], rejections: [], jsFunctions: []});

// --- Rows ------------------------------------------------------------------------------------------------------

/**
 * A state as the rows that cross the bridge, each `[kind, name, detail, number, number]`: one shape, so that they go as a
 * list of tuples with the existing types (ADR-003). An unknown duration is -1.
 */
export function toRows(state) {
    return [
        ['page', state.page.label, state.page.path, state.since, 0],
        ['ready', state.ready.state, state.ready.reason, 0, 0],
        ['connection', state.connection, '', 0, 0],
        ['counts', '', '', state.pendingTotal, state.rejectedTotal],
        ['jsCount', '', '', state.jsTotal, state.jsSaturated ? 1 : 0],
        ...state.pending.map(call => ['pending', call.name, '', call.id, call.elapsed ?? -1]),
        ...state.rejections.map(rejection => ['rejected', rejection.name, rejection.reason, rejection.id, rejection.at]),
        ...state.jsFunctions.map(called => ['js', called.name, '', called.id, called.calls]),
    ];
}

const unreadable = () => new Error('The inspectors gave a state of the bridge that this environment does not read');

/** The state that `rows` describe; throws when they are not rows of a state. Rows of a kind it does not know are ignored. */
export function fromRows(rows) {
    if (!Array.isArray(rows)) throw unreadable();
    const state = emptyState();
    for (const row of rows) {
        if (!Array.isArray(row) || row.length !== 5) throw unreadable();
        const [kind, name, detail, first, second] = row;
        if (typeof kind !== 'string' || typeof name !== 'string' || typeof detail !== 'string' || !Number.isFinite(first) || !Number.isFinite(second))
            throw unreadable();
        if (kind === 'page') Object.assign(state, {page: {label: name, path: detail}, since: first});
        else if (kind === 'ready') state.ready = {state: name, reason: detail};
        else if (kind === 'connection') state.connection = name;
        else if (kind === 'counts') Object.assign(state, {pendingTotal: first, rejectedTotal: second});
        else if (kind === 'jsCount') Object.assign(state, {jsTotal: first, jsSaturated: second !== 0});
        else if (kind === 'pending') state.pending.push({id: first, name, elapsed: second < 0 ? null : second});
        else if (kind === 'rejected') state.rejections.push({id: first, name, reason: detail, at: second});
        else if (kind === 'js') state.jsFunctions.push({id: first, name, calls: second});
    }
    return state;
}

// --- Relay -----------------------------------------------------------------------------------------------------

/**
 * Publishes the state of `monitor` to C++ for the detached panels of the page `host`, only while one reads. `announce()`
 * tells C++ that this page can publish, and starts when a panel already waits; C++ asks later through the page's
 * `wfInspect.publish()`. Each publication is answered by whether a panel still reads: when none does, the publications stop
 * and the monitor's probe leaves the bridge at the end of its lease. A library without the relay leaves the page silent.
 */
export function createBridgePublisher({monitor, bridge, host, view, period = publishPeriod}) {
    let running = false, timer = null, stopped = false;
    async function publish() {
        timer = null;
        let wanted = false;
        let forgotten = false;
        try { wanted = await bridge.cppFunction(publishFunction)(host, bridge.list(toRows(monitor.read()))); }
        catch (error) {
            // The connection is gone, or the page is no longer admitted: a later request starts again. A page that the relay
            // forgot (it keeps a bounded number of them) announces itself again.
            forgotten = error?.code === 'webfront.inspector.unknown';
        }
        if (wanted === true && !stopped) timer = view.setTimeout(publish, period);
        else running = false;
        if (forgotten && !stopped) announce();
    }
    async function announce() {
        try { if (await bridge.cppFunction(sourceFunction)(host) === true) start(); }
        catch { /* an older library: the attached panel still reads the monitor */ }
    }
    function start() {
        if (running || stopped) return;
        running = true;
        publish();
    }
    view[publisherGlobal] = {publish: start};
    return {
        get running() { return running; },
        start,
        announce,
        stop() {
            stopped = true;
            view.clearTimeout(timer);
            timer = null;
            running = false;
        },
    };
}

/** How many times a read starts again because the page published between two pages, before it gives up. */
export const readingAttempts = 8;

/**
 * The state that the page `host` published, read page after page: `{state, age, source, published}`, where `age` is how
 * long ago it was published in milliseconds, `source` whether that page is connected and `published` whether it published
 * yet (`state` is null until then). A publication that arrives between two pages starts the read again; null after
 * `readingAttempts` such changes, or once `active()` is false.
 */
export async function readPublishedState(bridge, host, {active = () => true} = {}) {
    for (let attempt = 0; attempt < readingAttempts; ++attempt) {
        const rows = [];
        let cursor = 0, publication = null, age = 0, flags = 0, stable = true;
        for (;;) {
            if (!active()) return null;
            const answer = await bridge.cppFunction(bridgeFunction)(host, cursor, 200);
            if (!Array.isArray(answer) || answer.length !== 5 || !Array.isArray(answer[0]) || !answer.slice(1).every(Number.isFinite)) throw unreadable();
            const [list, next, number] = answer;
            if (publication !== null && number !== publication) { stable = false; break; }
            publication = number;
            [, , , age, flags] = answer;
            rows.push(...list);
            if (!next) break;
            if (next <= cursor) throw unreadable();
            cursor = next;
        }
        if (!stable) continue;
        const published = (flags & 2) !== 0;
        return {state: published ? fromRows(rows) : null, age, source: (flags & 1) !== 0, published};
    }
    return null;
}

// --- Panel -----------------------------------------------------------------------------------------------------

export const pendingColumns = [
    {key: 'name', label: 'C++ function', text: row => row.name, className: 'name'},
    {key: 'elapsed', label: 'Pending for', text: row => formatDuration(row.elapsed), numeric: true,
        title: row => row.elapsed === null ? 'Called before an inspector read this page: when is not known' : ''},
];

export const rejectionColumns = [
    {key: 'at', label: 'Time', text: row => formatTime(row.at), numeric: true},
    {key: 'name', label: 'C++ function', text: row => row.name, className: 'name'},
    {key: 'reason', label: 'Reason', text: row => row.reason, className: 'error-text'},
];

export const calledColumns = [
    {key: 'name', label: 'JavaScript function', text: row => row.name, className: 'name'},
    {key: 'calls', label: 'Calls', text: row => String(row.calls), numeric: true},
];

/** A time of day as hours, minutes, seconds and milliseconds. */
export function formatTime(milliseconds) {
    const date = new Date(milliseconds);
    if (Number.isNaN(date.getTime())) return '–';
    const two = value => String(value).padStart(2, '0');
    return `${two(date.getHours())}:${two(date.getMinutes())}:${two(date.getSeconds())}.${String(date.getMilliseconds()).padStart(3, '0')}`;
}

const readyTexts = {
    pending: 'pending: the handshake is not acknowledged yet',
    resolved: 'resolved',
    rejected: 'rejected',
    none: 'this page has no WebFront bridge',
};

/** What the panel says of webFront.ready. */
export const describeReady = ready => ready.state === 'rejected' && ready.reason ? `rejected: ${ready.reason}` : readyTexts[ready.state] ?? ready.state;

export const bridgeStyles = `
dl.bridge{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0 0 12px}
dl.bridge dt{color:#a9b3c4}
dl.bridge dd{margin:0;overflow-wrap:anywhere}
dl.bridge dd.rejected{color:#ffbd2e}`;

/** A published state older than this, in milliseconds, is said to be old: its page stopped publishing. */
export const staleAfter = 3000;

/**
 * The panel of the bridge. `source()` resolves with `{state, age, source, published}` as readPublishedState does, or null
 * when nothing was read: the attached panel gives the monitor of its host page, the detached one the relay.
 */
export async function showBridgePanel({page, view, header, body, status, source}) {
    let search = '', failed = false, data = null, last = null;
    const summary = page.createElement('dl');
    summary.className = 'bridge';
    const field = label => {
        const term = page.createElement('dt'), value = page.createElement('dd');
        term.textContent = label;
        summary.append(term, value);
        return value;
    };
    const fields = {page: field('Page'), ready: field('webFront.ready'), connection: field('Connection'), counts: field('Calls to C++')};
    const pending = createTable(page, {columns: pendingColumns, caption: 'Pending calls to C++ (cppFunction)', sort: 'elapsed', descending: true});
    const rejections = createTable(page, {columns: rejectionColumns, caption: 'Promises rejected lately', sort: 'at', descending: true});
    const called = createTable(page, {columns: calledColumns, caption: 'JavaScript functions called by C++, while an inspector reads',
        sort: 'calls', descending: true});
    const show = () => {
        if (!data?.state) return;
        const {state} = data;
        fields.page.textContent = state.page.label ? `${state.page.label} (${state.page.path})` : state.page.path || '–';
        fields.ready.textContent = describeReady(state.ready);
        fields.ready.classList.toggle('rejected', state.ready.state === 'rejected');
        fields.connection.textContent = state.connection;
        fields.counts.textContent = `${state.pendingTotal} pending, ${state.rejectedTotal} rejected since ${formatTime(state.since)}`;
        pending.render(filterRows(state.pending, search, ['name']));
        rejections.render(filterRows(state.rejections, search, ['name', 'reason']));
        called.render(filterRows(state.jsFunctions, search, ['name']));
        tell();
    };
    // The status tells the last read, whatever is shown: a failed read stays told until a read succeeds, and a page that
    // is gone or has not published stays told while the rows read earlier are kept and searched.
    const tell = () => {
        if (failed || !last) return;
        status.classList.remove('error');
        if (!last.state) {
            status.textContent = last.source ? 'Waiting for the page to publish the state of its bridge…'
                : 'The page that opened this panel is not connected: its bridge cannot be read.';
            return;
        }
        const {state} = last;
        const hidden = [state.pendingTotal > state.pending.length ? `${state.pendingTotal - state.pending.length} more pending calls` : '',
            state.jsTotal > state.jsFunctions.length
                ? `${state.jsSaturated ? 'at least ' : ''}${state.jsTotal - state.jsFunctions.length} more JavaScript functions` : ''].filter(Boolean);
        status.textContent = [last.age > staleAfter ? `The page published this state ${formatDuration(last.age)} ago.` : '',
            hidden.length ? `Not shown: ${hidden.join(', ')}.` : ''].filter(Boolean).join(' ');
    };
    const toolbar = createToolbar(page, view, {searchLabel: 'Search the calls', onSearch: text => { search = text; show(); }});
    header.insertBefore(toolbar.pause, header.children[1] ?? null);
    const box = table => {
        const scroll = page.createElement('div');
        scroll.className = 'scroll';
        scroll.append(table.element);
        return scroll;
    };
    body.append(toolbar.element, summary, box(pending), box(rejections), box(called));
    const poller = createPoller({
        read: ({active}) => source({active}),
        onData: read => {
            failed = false;
            last = read;
            if (read.state) data = read;
            if (data) show(); else tell();
        },
        onError: error => {
            failed = true;
            status.classList.add('error');
            status.textContent = readFailure(error, 'The state of the bridge');
        },
        period: toolbar.chosen,
        timers: view,
    });
    toolbar.attach(poller);
    view.addEventListener('pagehide', () => poller.stop());
    await poller.start();
    return {poller, toolbar, summary, fields, pending, rejections, called};
}

/**
 * Where a panel reads the state: in a frame, the monitor of the page that holds it, through the same rows as the relay so
 * that both show the same thing; detached, what that page published to C++.
 */
export function bridgeSource({view, bridge, framed, host}) {
    if (framed) return async () => {
        const monitor = view.parent.webfrontInspectorHost?.bridgeState;
        if (!monitor) throw new Error('The page that holds this panel does not follow its bridge');
        return {state: fromRows(toRows(monitor.read())), age: 0, source: true, published: true};
    };
    return ({active}) => {
        if (!host) throw new Error('This window does not name the page whose bridge it shows: open the panel from the menu');
        return readPublishedState(bridge, host, {active});
    };
}
