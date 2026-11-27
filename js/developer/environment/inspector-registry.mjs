// The panels of the C++ registry (#681, ADR-027 section 7): the functions that the application registered with their
// signature and their activity, the JavaScript functions that C++ calls, the connected browsers, the tasks of the server's
// scope and the application's terminal commands. Each is read by pages from webfront.inspector.*, at a chosen period, and
// shown in a table that can be sorted and searched.

// --- Rows ------------------------------------------------------------------------------------------------------

export const functionsFunction = 'webfront.inspector.functions';
export const jsFunctionsFunction = 'webfront.inspector.jsFunctions';
export const browsersFunction = 'webfront.inspector.browsers';
export const commandsFunction = 'webfront.commands.list';

/** The periods the panels offer, in milliseconds; the first is the default (ADR-027: 250 ms). */
export const periods = [250, 1000, 5000];

const count = value => Number.isSafeInteger(value) && value >= 0;
const duration = value => typeof value === 'number' && Number.isFinite(value) && value >= 0;
const texts = (...values) => values.every(value => typeof value === 'string');

/** A row of webfront.inspector.functions as an object; null when it is not one. */
export function readFunctionRow(tuple) {
    if (!Array.isArray(tuple) || tuple.length !== 11) return null;
    const [id, name, signature, location, scope, flags, calls, failures, lastError, mean, longest] = tuple;
    if (!count(id) || id === 0 || !texts(name, signature, location, scope, lastError) || !count(flags) || !count(calls) || !count(failures)
        || !duration(mean) || !duration(longest)) return null;
    const timed = (flags & 4) !== 0;
    return {id, name, signature, location, scope, takesCaller: (flags & 1) !== 0, deferred: (flags & 2) !== 0, timed, calls, failures, lastError,
        // Without a timed call the durations are unknown, not zero: they sort after every measured one.
        mean: timed ? mean : null, longest: timed ? longest : null};
}

/** A row of webfront.inspector.jsFunctions as an object; null when it is not one. */
export function readJsFunctionRow(tuple) {
    if (!Array.isArray(tuple) || tuple.length !== 4) return null;
    const [id, name, calls, failures] = tuple;
    if (!count(id) || id === 0 || typeof name !== 'string' || !count(calls) || !count(failures)) return null;
    return {id, name, calls, failures};
}

/** A row of webfront.inspector.browsers as an object; null when it is not one. */
export function readBrowserRow(tuple) {
    if (!Array.isArray(tuple) || tuple.length !== 7) return null;
    const [id, flags, path, space, pendingWrites, pendingResults, pendingCalls] = tuple;
    if (!count(id) || !count(flags) || !texts(path, space) || !count(pendingWrites) || !count(pendingResults) || !count(pendingCalls)) return null;
    return {id, embeddedView: (flags & 1) !== 0, localClient: (flags & 2) !== 0, started: (flags & 4) !== 0, closed: (flags & 8) !== 0,
        caller: (flags & 16) !== 0, path, space, pendingWrites, pendingResults, pendingCalls};
}

/** How many pages a listing reads before it gives up: a cursor that does not advance must not loop for ever. */
export const listingPages = 1000;

const unreadable = () => new Error('The inspectors gave a page that this environment does not read');

/**
 * Every row of a paged function, page after page: `name(cursor, 200)` answers `[rows, next, ...rest]`, and `next` is the
 * cursor of the following page, 0 at the end. Resolves with `{rows, rest}`, `rest` being what the first page gave after
 * its cursor (the number of tasks, for the browsers). `active()` is asked before each page: once false, the listing stops
 * and resolves with null. Throws for an answer that is not a page, or whose cursor does not advance.
 */
export async function listRows(bridge, name, readRow, {active = () => true} = {}) {
    const rows = [];
    let cursor = 0, rest = [];
    for (let page = 0; page < listingPages; ++page) {
        if (!active()) return null;
        const answer = await bridge.cppFunction(name)(cursor, 200);
        if (!Array.isArray(answer) || answer.length < 2 || !Array.isArray(answer[0]) || !count(answer[1])) throw unreadable();
        const read = answer[0].map(readRow);
        if (read.includes(null)) throw unreadable();
        rows.push(...read);
        if (page === 0) rest = answer.slice(2);
        const next = answer[1];
        if (next === 0) return {rows, rest};
        if (next <= cursor) throw unreadable();
        cursor = next;
    }
    throw unreadable();
}

/** The application's terminal commands, [{name, help}]; empty when it registers none (no webfront::Commands). */
export async function listCommands(bridge) {
    let json;
    try { json = await bridge.cppFunction(commandsFunction)(); }
    catch (error) {
        if (error?.code === 'webfront.notFound') return [];
        throw error;
    }
    const parsed = JSON.parse(json);
    if (!Array.isArray(parsed)) throw unreadable();
    return parsed.filter(command => typeof command?.name === 'string').map(({name, help}) => ({name, help: typeof help === 'string' ? help : ''}));
}

// --- Sorting, searching, formatting ----------------------------------------------------------------------------

/**
 * `rows` sorted by `key`, ascending or descending: numbers by value, texts without regard to case, booleans false first.
 * A value that is unknown (null) comes last in both directions, and rows that compare equal keep the order of their
 * identifiers, so that a table whose values do not change does not move.
 */
export function sortRows(rows, key, descending = false) {
    const direction = descending ? -1 : 1;
    const compare = (a, b) => {
        const left = a[key], right = b[key];
        if (left === null || left === undefined || right === null || right === undefined) {
            if ((left ?? null) === (right ?? null)) return 0;
            return (left ?? null) === null ? 1 : -1;
        }
        if (typeof left === 'string') return direction * left.localeCompare(right, undefined, {sensitivity: 'base', numeric: true});
        return direction * (Number(left) - Number(right));
    };
    return [...rows].sort((a, b) => compare(a, b) || a.id - b.id);
}

/** The rows of which one of `fields` contains every word of `search`, without regard to case; all of them for an empty search. */
export function filterRows(rows, search, fields) {
    const words = search.toLowerCase().split(/\s+/).filter(Boolean);
    if (!words.length) return rows;
    return rows.filter(row => {
        const text = fields.map(field => String(row[field] ?? '')).join('\n').toLowerCase();
        return words.every(word => text.includes(word));
    });
}

/** Whether a function is one of WebFront's own (the environment's callbacks, the terminal's commands, the DOM bridge). */
export const ownFunction = name => name.startsWith('webfront.') || name.startsWith('wfDom.') || name.startsWith('wfBind.');

/** A duration in milliseconds as a short text: microseconds under a millisecond, seconds from a second; "–" when unknown. */
export function formatDuration(milliseconds) {
    if (milliseconds === null || milliseconds === undefined) return '–';
    if (milliseconds < 1) return `${Math.round(milliseconds * 1000)} µs`;
    if (milliseconds < 10) return `${milliseconds.toFixed(2)} ms`;
    if (milliseconds < 1000) return `${Math.round(milliseconds)} ms`;
    return `${(milliseconds / 1000).toFixed(2)} s`;
}

export const formatPeriod = period => period < 1000 ? `${period} ms` : `${period / 1000} s`;

// --- Periodic reads --------------------------------------------------------------------------------------------

/**
 * Reads at a period: `read({active})` is awaited, its result given to `onData` (unless it is null, or the poller was paused
 * or stopped meanwhile), a failure to `onError`, then the next read is scheduled `period` later. One read at a time: a slow
 * read delays the next one, it is never doubled. `setPeriod` applies from the next wait; `pause`, `resume`, `stop`.
 */
export function createPoller({read, onData, onError = () => {}, period = periods[0], timers = globalThis}) {
    let timer = null, paused = false, stopped = false, running = null, current = period;
    const active = () => !paused && !stopped;
    async function once() {
        const data = await read({active});
        if (active() && data !== null && data !== undefined) onData(data);
    }
    function run() {
        running = once().catch(error => { if (active()) onError(error); }).finally(() => {
            running = null;
            schedule();
        });
        return running;
    }
    function schedule() {
        if (!active() || timer !== null || running !== null) return;
        timer = timers.setTimeout(() => {
            timer = null;
            run();
        }, current);
    }
    const cancel = () => {
        timers.clearTimeout(timer);
        timer = null;
    };
    return {
        get paused() { return paused; },
        get period() { return current; },
        /** Reads now, then at each period. */
        start: () => run(),
        setPeriod(value) {
            current = value;
            if (timer !== null) {
                cancel();
                schedule();
            }
        },
        pause() {
            paused = true;
            cancel();
        },
        resume() {
            if (!paused) return;
            paused = false;
            schedule();
        },
        stop() {
            stopped = true;
            cancel();
        },
        /** Resolves once the read in progress, if any, is over. */
        settled: () => running ?? Promise.resolve(),
    };
}

// --- Tables ----------------------------------------------------------------------------------------------------

/**
 * A table whose columns sort: `columns` are {key, label, text(row), title(row)?, numeric?, className?}. `render(rows)`
 * shows the rows sorted by the chosen column, each kept by its identifier so that an update changes only the cells whose
 * text changed and moves a row only when the order changes. A click on a header sorts by it, a second click reverses.
 */
export function createTable(page, {columns, caption, sort = columns[0].key, descending = false, onSort = () => {}}) {
    const table = page.createElement('table');
    table.className = 'registry';
    const title = page.createElement('caption');
    title.textContent = caption;
    const head = page.createElement('thead');
    const headRow = page.createElement('tr');
    const body = page.createElement('tbody');
    const headers = new Map();
    const state = {sort, descending};
    let shown = [];
    const mark = () => {
        for (const [key, header] of headers)
            header.setAttribute('aria-sort', key === state.sort ? (state.descending ? 'descending' : 'ascending') : 'none');
    };
    for (const column of columns) {
        const header = page.createElement('th');
        header.scope = 'col';
        if (column.numeric) header.className = 'numeric';
        const button = page.createElement('button');
        button.type = 'button';
        button.textContent = column.label;
        button.addEventListener('click', () => {
            if (state.sort === column.key) state.descending = !state.descending;
            else {
                state.sort = column.key;
                // Counts and durations are read from the largest: that is what one looks for.
                state.descending = Boolean(column.numeric);
            }
            mark();
            render(shown);
            onSort({...state});
        });
        header.append(button);
        headers.set(column.key, header);
        headRow.append(header);
    }
    head.append(headRow);
    table.append(title, head, body);
    mark();

    function render(rows) {
        shown = rows;
        const sorted = sortRows(rows, state.sort, state.descending);
        const existing = new Map([...body.children].map(row => [row.dataset.id, row]));
        const kept = new Set(sorted.map(row => String(row.id)));
        for (const [id, row] of existing) if (!kept.has(id)) row.remove();
        sorted.forEach((data, index) => {
            const id = String(data.id);
            let row = existing.get(id);
            if (!row) {
                row = page.createElement('tr');
                row.dataset.id = id;
                for (const column of columns) {
                    const cell = page.createElement('td');
                    cell.className = [column.numeric ? 'numeric' : '', column.className ?? ''].join(' ').trim();
                    row.append(cell);
                }
            }
            columns.forEach((column, cell) => {
                const text = column.text(data);
                if (row.children[cell].textContent !== text) row.children[cell].textContent = text;
                const hint = column.title?.(data) ?? '';
                if (row.children[cell].title !== hint) row.children[cell].title = hint;
            });
            if (body.children[index] !== row) body.insertBefore(row, body.children[index] ?? null);
        });
    }
    return {element: table, render, get sort() { return {...state}; }, get rows() { return [...body.children]; }};
}

export const functionColumns = [
    {key: 'name', label: 'Name', text: row => row.scope ? `${row.name}  (${row.scope})` : row.name, className: 'name'},
    {key: 'signature', label: 'Signature', text: row => row.signature, className: 'code'},
    {key: 'calls', label: 'Calls', text: row => String(row.calls), numeric: true},
    {key: 'failures', label: 'Failures', text: row => String(row.failures), numeric: true},
    {key: 'mean', label: 'Mean', text: row => formatDuration(row.mean), numeric: true,
        title: row => row.timed ? '' : 'No call was timed: durations are measured while this panel is open'},
    {key: 'longest', label: 'Longest', text: row => formatDuration(row.longest), numeric: true},
    {key: 'takesCaller', label: 'Caller', text: row => row.takesCaller ? 'UI' : '–',
        title: row => row.takesCaller ? 'Receives the calling UI: it can check ui.isLocalClient()'
            : 'Does not receive the calling UI: it cannot check ui.isLocalClient()'},
    {key: 'deferred', label: 'Answer', text: row => row.deferred ? 'later' : 'at once',
        title: row => row.deferred ? 'Returns a task (ResultTask): the Promise settles when it completes' : ''},
    {key: 'location', label: 'Registered at', text: row => row.location, className: 'location'},
    {key: 'lastError', label: 'Last error', text: row => row.lastError, className: 'error-text'},
];

export const jsFunctionColumns = [
    {key: 'name', label: 'Name', text: row => row.name, className: 'name'},
    {key: 'calls', label: 'Calls', text: row => String(row.calls), numeric: true},
    {key: 'failures', label: 'Failures of typed calls', text: row => String(row.failures), numeric: true},
];

const browserState = row => row.closed ? 'closed' : row.started ? 'started' : 'starting';

export const browserColumns = [
    {key: 'id', label: 'Id', text: row => row.caller ? `${row.id} (this page)` : String(row.id), numeric: true},
    {key: 'embeddedView', label: 'Kind', text: row => row.embeddedView ? 'embedded view' : 'external browser',
        title: row => row.embeddedView ? 'Proved that it is a view of the application' : 'Did not prove that it is a view of the application'},
    {key: 'localClient', label: 'Local client', text: row => row.localClient ? 'yes' : 'no',
        title: row => row.localClient ? 'ui.isLocalClient() is true: admitted to the inspectors and to the system services'
            : 'ui.isLocalClient() is false'},
    {key: 'state', label: 'State', text: browserState},
    {key: 'path', label: 'Path', text: row => row.path, className: 'code'},
    {key: 'space', label: 'Space', text: row => row.space || '–', className: 'code'},
    {key: 'pendingWrites', label: 'Pending writes', text: row => String(row.pendingWrites), numeric: true,
        title: () => 'Frames queued for this browser and not yet written to the network'},
    {key: 'pendingResults', label: 'Awaited results', text: row => String(row.pendingResults), numeric: true,
        title: () => 'Typed calls of C++ (jsFunction<R>) whose result this browser has not returned'},
    {key: 'pendingCalls', label: 'Unanswered calls', text: row => String(row.pendingCalls), numeric: true,
        title: () => 'Calls of this browser that a C++ callback has not answered yet'},
];

export const commandColumns = [
    {key: 'name', label: 'Command', text: row => row.name, className: 'name'},
    {key: 'help', label: 'Help', text: row => row.help},
];

export const registryStyles = `
.toolbar{display:flex;flex-wrap:wrap;align-items:center;gap:8px 12px;margin:0 0 8px}
.toolbar input[type=search]{flex:1 1 160px;min-width:0;min-height:32px;padding:0 8px;border:1px solid #46536a;border-radius:4px;background:#0f131b;color:inherit;font:inherit}
.toolbar select{min-height:32px;padding:0 6px;border:1px solid #46536a;border-radius:4px;background:#2f3a50;color:inherit;font:inherit}
.toolbar label{display:flex;align-items:center;gap:6px;color:#a9b3c4;font-size:12px}
.toolbar :focus-visible{outline:2px solid #d7fa85;outline-offset:1px}
.scroll{overflow-x:auto;margin:0 0 16px}
table.registry{border-collapse:collapse;width:100%;font-size:13px}
table.registry caption{text-align:left;font-weight:600;padding:4px 0}
table.registry th,table.registry td{padding:4px 8px;border-bottom:1px solid #2f3a50;text-align:left;vertical-align:top;white-space:nowrap}
table.registry th{position:sticky;top:0;background:#202737;padding:0}
table.registry th button{width:100%;min-height:32px;padding:0 8px;border:0;background:none;color:inherit;font:inherit;font-weight:600;text-align:inherit;cursor:pointer}
table.registry th button:focus-visible{outline:2px solid #d7fa85;outline-offset:-2px}
table.registry th[aria-sort=ascending] button::after{content:" ▲";color:#d7fa85}
table.registry th[aria-sort=descending] button::after{content:" ▼";color:#d7fa85}
table.registry .numeric{text-align:right;font-variant-numeric:tabular-nums}
table.registry .name{font-weight:600}
table.registry .code{font-family:ui-monospace,monospace;font-size:12px;color:#d7fa85}
table.registry .location{color:#a9b3c4;font-size:12px}
table.registry .error-text{color:#ffbd2e;white-space:normal;overflow-wrap:anywhere;min-width:12em}
.summary{margin:0 0 8px}`;

// --- Panels ----------------------------------------------------------------------------------------------------

const periodKey = 'webfront.inspector.period';

/** The period this viewer chose last, among those offered; the default when none is kept or the storage is unavailable. */
export function keptPeriod(view) {
    try {
        const kept = Number(view.localStorage?.getItem(periodKey));
        return periods.includes(kept) ? kept : periods[0];
    } catch { return periods[0]; }
}

export function keepPeriod(view, period) {
    try { view.localStorage?.setItem(periodKey, String(period)); }
    catch { /* a convenience only: the choice lasts as long as the page */ }
}

/** The message of a failed read: why a page that is not admitted is refused, else what failed. */
export function readFailure(error, what) {
    return error?.code === 'webfront.inspector.denied'
        ? 'The inspectors answer only a view of the application: this page is not admitted.'
        : error?.code === 'webfront.notFound'
            ? 'The WebFront library of this application does not have this inspector: update it.'
            : `${what} cannot be read: ${error?.message ?? error}`;
}

/**
 * The controls shared by the panels: a search field, the period of the reads and a pause. `onSearch(text)` follows the
 * field; the period and the pause drive `poller` once `attach(poller)` gave it.
 */
export function createToolbar(page, view, {onSearch, searchLabel}) {
    const bar = page.createElement('div');
    bar.className = 'toolbar';
    const search = page.createElement('input');
    search.type = 'search';
    search.placeholder = searchLabel;
    search.setAttribute('aria-label', searchLabel);
    search.addEventListener('input', () => onSearch(search.value));
    const periodLabel = page.createElement('label');
    periodLabel.textContent = 'Refresh every ';
    const period = page.createElement('select');
    period.className = 'period';
    const chosen = keptPeriod(view);
    for (const value of periods) {
        const option = page.createElement('option');
        option.value = String(value);
        option.textContent = formatPeriod(value);
        option.selected = value === chosen;
        period.append(option);
    }
    periodLabel.append(period);
    const pause = page.createElement('button');
    pause.type = 'button';
    pause.className = 'pause';
    pause.textContent = 'Pause';
    bar.append(search, periodLabel);
    let poller = null;
    period.addEventListener('change', () => {
        const value = Number(period.value);
        keepPeriod(view, value);
        poller?.setPeriod(value);
    });
    pause.addEventListener('click', () => {
        if (!poller) return;
        if (poller.paused) poller.resume(); else poller.pause();
        pause.textContent = poller.paused ? 'Resume' : 'Pause';
    });
    return {element: bar, search, period, pause, chosen, attach(value) { poller = value; }};
}

const scrolling = (page, table) => {
    const box = page.createElement('div');
    box.className = 'scroll';
    box.append(table.element);
    return box;
};

/**
 * The panel of the functions: the C++ functions with their signature and their counters, then the JavaScript functions that
 * C++ called. WebFront's own functions (the environment's callbacks, the terminal's) are hidden unless asked for.
 */
export async function showFunctionsPanel({page, view, header, body, status, bridge}) {
    let search = '', own = false, failed = false, data = {functions: [], jsFunctions: []};
    const functions = createTable(page, {columns: functionColumns, caption: 'C++ functions (cppFunction)'});
    const jsFunctions = createTable(page, {columns: jsFunctionColumns, caption: 'JavaScript functions called by C++ (jsFunction)'});
    const show = () => {
        const visible = own ? data.functions : data.functions.filter(row => !ownFunction(row.name));
        const found = filterRows(visible, search, ['name', 'scope', 'signature', 'location', 'lastError']);
        functions.render(found);
        const calls = filterRows(own ? data.jsFunctions : data.jsFunctions.filter(row => !ownFunction(row.name)), search, ['name']);
        jsFunctions.render(calls);
        // A failed read stays told until a read succeeds: filtering what was read earlier must not hide it.
        if (failed) return;
        status.classList.remove('error');
        const hidden = data.functions.length - visible.length;
        status.textContent = `${found.length} of ${visible.length} C++ functions`
            + (hidden ? `, ${hidden} of WebFront's own hidden` : '') + `; ${calls.length} JavaScript functions called.`;
    };
    const toolbar = createToolbar(page, view, {searchLabel: 'Search the functions', onSearch: text => { search = text; show(); }});
    const ownLabel = page.createElement('label');
    const ownBox = page.createElement('input');
    ownBox.type = 'checkbox';
    ownBox.className = 'own';
    ownBox.addEventListener('change', () => { own = ownBox.checked; show(); });
    ownLabel.append(ownBox, 'WebFront\'s own functions');
    toolbar.element.append(ownLabel);
    header.insertBefore(toolbar.pause, header.children[1] ?? null);
    body.append(toolbar.element, scrolling(page, functions), scrolling(page, jsFunctions));
    const poller = createPoller({
        async read({active}) {
            const listed = await listRows(bridge, functionsFunction, readFunctionRow, {active});
            if (!listed) return null;
            const called = await listRows(bridge, jsFunctionsFunction, readJsFunctionRow, {active});
            return called && {functions: listed.rows, jsFunctions: called.rows};
        },
        onData: read => { data = read; failed = false; show(); },
        onError: error => {
            failed = true;
            status.classList.add('error');
            status.textContent = readFailure(error, 'The functions');
        },
        period: toolbar.chosen,
        timers: view,
    });
    toolbar.attach(poller);
    view.addEventListener('pagehide', () => poller.stop());
    await poller.start();
    return {poller, toolbar, functions, jsFunctions, own: ownBox};
}

/** The panel of the browsers and the tasks: who is connected and what waits, the tasks of the server's scope, the commands. */
export async function showBrowsersPanel({page, view, header, body, status, bridge}) {
    let search = '', failed = false, data = {browsers: [], tasks: 0, commands: []};
    const browsers = createTable(page, {columns: browserColumns, caption: 'Connected browsers'});
    const commands = createTable(page, {columns: commandColumns, caption: 'Terminal commands of the application (webfront::Commands)'});
    const summary = page.createElement('p');
    summary.className = 'summary';
    const show = () => {
        browsers.render(filterRows(data.browsers.map(row => ({...row, state: browserState(row)})), search, ['id', 'path', 'space', 'state']));
        // The commands have no identifier of their own: their rank in the list, which is sorted by name, is one.
        commands.render(filterRows(data.commands.map((command, index) => ({...command, id: index + 1})), search, ['name', 'help']));
        summary.textContent = `${data.browsers.length} connected ${data.browsers.length === 1 ? 'browser' : 'browsers'}, `
            + `${data.tasks} running ${data.tasks === 1 ? 'task' : 'tasks'} in the server's scope, `
            + `${data.commands.length} terminal ${data.commands.length === 1 ? 'command' : 'commands'}.`;
        // A failed read stays told until a read succeeds.
        if (failed) return;
        status.classList.remove('error');
        status.textContent = '';
    };
    const toolbar = createToolbar(page, view, {searchLabel: 'Search the browsers and the commands', onSearch: text => { search = text; show(); }});
    header.insertBefore(toolbar.pause, header.children[1] ?? null);
    body.append(toolbar.element, summary, scrolling(page, browsers), scrolling(page, commands));
    const poller = createPoller({
        async read({active}) {
            const listed = await listRows(bridge, browsersFunction, readBrowserRow, {active});
            if (!listed) return null;
            const tasks = listed.rest[0];
            if (!count(tasks)) throw unreadable();
            return {browsers: listed.rows, tasks, commands: await listCommands(bridge)};
        },
        onData: read => { data = read; failed = false; show(); },
        onError: error => {
            failed = true;
            status.classList.add('error');
            status.textContent = readFailure(error, 'The browsers');
        },
        period: toolbar.chosen,
        timers: view,
    });
    toolbar.attach(poller);
    view.addEventListener('pagehide', () => poller.stop());
    await poller.start();
    return {poller, toolbar, browsers, commands, summary};
}
