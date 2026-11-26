// Specs of the registry panels of the inspectors (#681, ADR-027 section 7): the rows of the functions, of the JavaScript
// calls and of the browsers, their paged reading, sorting, searching, and the periodic reads. The functions of a real
// application are read in test/web/suite/specs/webfront-specs.mjs.
import {readFunctionRow, readJsFunctionRow, readBrowserRow, listRows, listCommands, listingPages, sortRows, filterRows, ownFunction,
    formatDuration, formatPeriod, periods, createPoller, createTable, functionColumns, keptPeriod, readFailure,
    showFunctionsPanel, showBrowsersPanel, functionsFunction, jsFunctionsFunction, browsersFunction,
    commandsFunction} from '../../../js/developer/environment/inspector-registry.mjs';
import {main} from '../../../js/developer/environment/inspector.mjs';

const functionTuple = (id, name, {signature = '() => Promise<void>', location = `app.cpp:${id}`, scope = '', flags = 0, calls = 0, failures = 0,
    lastError = '', mean = 0, longest = 0} = {}) => [id, name, signature, location, scope, flags, calls, failures, lastError, mean, longest];

/** Timers that run only when the spec says so: `fire()` runs the pending one, `delays` are those that were asked. */
function manualTimers() {
    let pending = null, next = 1;
    const delays = [];
    return {
        delays,
        get pending() { return pending !== null; },
        setTimeout(run, delay) {
            delays.push(delay);
            pending = {id: next, run};
            return next++;
        },
        clearTimeout(id) { if (pending?.id === id) pending = null; },
        fire() {
            const {run} = pending;
            pending = null;
            run();
        },
    };
}

/** A bridge that answers each function from `answers[name](...arguments)`, and records the calls. */
function fakeBridge(answers) {
    const calls = [];
    return {calls, cppFunction: name => async (...args) => {
        calls.push([name, ...args]);
        if (!answers[name]) throw Object.assign(new Error(`C++ function '${name}' was not found`), {code: 'webfront.notFound'});
        return answers[name](...args);
    }};
}

/** Pages of `size` rows of `tuples`, by a cursor that is the identifier of the last row served, as C++ answers. */
const paged = (tuples, size, ...rest) => (after, count) => {
    const remaining = tuples().filter(([id]) => id > after);
    const served = remaining.slice(0, Math.min(size, count));
    return [served, remaining.length > served.length ? served.at(-1)[0] : 0, ...rest];
};

/** A view for a panel: the real document's elements, its own timers, storage and listeners. */
function panelView(timers, storage = new Map()) {
    const listeners = new Map();
    return {
        setTimeout: (run, delay) => timers.setTimeout(run, delay),
        clearTimeout: id => timers.clearTimeout(id),
        localStorage: {getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value)},
        addEventListener: (name, handler) => listeners.set(name, handler),
        listeners,
        storage,
    };
}

function panelParts() {
    const header = document.createElement('header');
    header.append(document.createElement('h1'));
    const body = document.createElement('main');
    const status = document.createElement('p');
    body.append(status);
    return {page: document, header, body, status};
}

const cells = row => [...row.children].map(cell => cell.textContent);
const column = (table, index) => table.rows.map(row => row.children[index].textContent);

describe('The registry panels of the inspectors', () => {
    describe('rows', () => {
        it('read a function with its flags, and leave unknown the durations that were never measured', () => {
            const row = readFunctionRow(functionTuple(3, 'save', {signature: '(a: string) => Promise<number>', location: 'app.cpp:12', scope: '/s/a/',
                flags: 1 | 2 | 4, calls: 9, failures: 2, lastError: 'disk full', mean: 1.5, longest: 4}));
            expect(row).toEqual({id: 3, name: 'save', signature: '(a: string) => Promise<number>', location: 'app.cpp:12', scope: '/s/a/',
                takesCaller: true, deferred: true, timed: true, calls: 9, failures: 2, lastError: 'disk full', mean: 1.5, longest: 4});
            const untimed = readFunctionRow(functionTuple(4, 'load', {calls: 3}));
            expect(untimed.timed).toBeFalse();
            expect(untimed.mean).toBeNull();
            expect(untimed.longest).toBeNull();
        });

        it('refuse what is not a row', () => {
            expect(readFunctionRow(null)).toBeNull();
            expect(readFunctionRow(functionTuple(0, 'zero'))).toBeNull();
            expect(readFunctionRow(functionTuple(1, 'short').slice(0, 10))).toBeNull();
            expect(readFunctionRow(functionTuple(1, 7))).toBeNull();
            expect(readFunctionRow(functionTuple(1, 'negative', {calls: -1}))).toBeNull();
            expect(readJsFunctionRow([1, 'draw', 2])).toBeNull();
            expect(readJsFunctionRow([1, 'draw', 2, 0])).toEqual({id: 1, name: 'draw', calls: 2, failures: 0});
            expect(readBrowserRow([0, 1, '/', '', 0, 0])).toBeNull();
        });

        it('read a browser, whose identifier may be 0', () => {
            expect(readBrowserRow([0, 1 | 2 | 4 | 16, '/', '', 1, 2, 3])).toEqual({id: 0, embeddedView: true, localClient: true, started: true,
                closed: false, caller: true, path: '/', space: '', pendingWrites: 1, pendingResults: 2, pendingCalls: 3});
            expect(readBrowserRow([7, 8, '/sub/', '/s/a/', 0, 0, 0])).toEqual(jasmine.objectContaining({id: 7, embeddedView: false, closed: true, space: '/s/a/'}));
        });
    });

    describe('paged reading', () => {
        it('follows the cursor to the end, and keeps what the first page gives beside its rows', async () => {
            const tuples = [1, 2, 3, 4, 5].map(id => functionTuple(id, `f${id}`));
            const bridge = fakeBridge({[functionsFunction]: paged(() => tuples, 2, 'rest')});
            const listed = await listRows(bridge, functionsFunction, readFunctionRow);
            expect(listed.rows.map(row => row.name)).toEqual(['f1', 'f2', 'f3', 'f4', 'f5']);
            expect(listed.rest).toEqual(['rest']);
            expect(bridge.calls).toEqual([[functionsFunction, 0, 200], [functionsFunction, 2, 200], [functionsFunction, 4, 200]]);
        });

        it('stops when the panel is paused, having read no further', async () => {
            const tuples = [1, 2, 3].map(id => functionTuple(id, `f${id}`));
            const bridge = fakeBridge({[functionsFunction]: paged(() => tuples, 1)});
            let reads = 0;
            expect(await listRows(bridge, functionsFunction, readFunctionRow, {active: () => ++reads <= 2})).toBeNull();
            expect(bridge.calls.length).toBe(2);
        });

        it('refuses a page it does not read, and a cursor that does not advance', async () => {
            await expectAsync(listRows(fakeBridge({[functionsFunction]: () => 'nope'}), functionsFunction, readFunctionRow)).toBeRejectedWithError(/does not read/);
            await expectAsync(listRows(fakeBridge({[functionsFunction]: () => [[[1, 2]], 0]}), functionsFunction, readFunctionRow))
                .toBeRejectedWithError(/does not read/);
            const stuck = fakeBridge({[functionsFunction]: () => [[functionTuple(1, 'f')], 1]});
            await expectAsync(listRows(stuck, functionsFunction, readFunctionRow)).toBeRejectedWithError(/does not read/);
            expect(stuck.calls.length).toBeLessThan(listingPages);
        });

        it('lists the commands of the application, and none when it registers no command', async () => {
            const bridge = fakeBridge({[commandsFunction]: () => JSON.stringify([{name: 'deploy', help: 'ships'}, {name: 'seed'}, {help: 'nameless'}])});
            expect(await listCommands(bridge)).toEqual([{name: 'deploy', help: 'ships'}, {name: 'seed', help: ''}]);
            expect(await listCommands(fakeBridge({}))).toEqual([]);
            const denied = fakeBridge({[commandsFunction]: () => { throw Object.assign(new Error('refused'), {code: 'webfront.inspector.denied'}); }});
            await expectAsync(listCommands(denied)).toBeRejectedWithError(/refused/);
        });
    });

    describe('sorting and searching', () => {
        const rows = [
            {id: 1, name: 'beta', calls: 5, mean: 2, takesCaller: true},
            {id: 2, name: 'Alpha', calls: 50, mean: null, takesCaller: false},
            {id: 3, name: 'gamma10', calls: 5, mean: 0.5, takesCaller: true},
            {id: 4, name: 'gamma2', calls: 0, mean: null, takesCaller: false},
        ];
        const ids = sorted => sorted.map(row => row.id);

        it('sort texts without regard to case and numbers by value, in both directions', () => {
            expect(ids(sortRows(rows, 'name'))).toEqual([2, 1, 4, 3]);
            expect(ids(sortRows(rows, 'name', true))).toEqual([3, 4, 1, 2]);
            expect(ids(sortRows(rows, 'calls', true))).toEqual([2, 1, 3, 4]);
            expect(ids(sortRows(rows, 'calls'))).toEqual([4, 1, 3, 2]);
            expect(ids(sortRows(rows, 'takesCaller', true))).toEqual([1, 3, 2, 4]);
        });

        it('keep the unknown durations last in both directions, and never change the rows given', () => {
            expect(ids(sortRows(rows, 'mean'))).toEqual([3, 1, 2, 4]);
            expect(ids(sortRows(rows, 'mean', true))).toEqual([1, 3, 2, 4]);
            expect(ids(rows)).toEqual([1, 2, 3, 4]);
        });

        it('search every word in the fields given, without regard to case', () => {
            const functions = [{id: 1, name: 'saveFile', signature: '(a: string) => Promise<void>', location: 'files.cpp:3'},
                {id: 2, name: 'loadFile', signature: '() => Promise<string>', location: 'files.cpp:9'},
                {id: 3, name: 'ping', signature: '() => Promise<void>', location: 'net.cpp:1'}];
            const names = (search, fields = ['name', 'signature', 'location']) => filterRows(functions, search, fields).map(row => row.name);
            expect(names('')).toEqual(['saveFile', 'loadFile', 'ping']);
            expect(names('FILE')).toEqual(['saveFile', 'loadFile']);
            expect(names('file void')).toEqual(['saveFile']);
            expect(names('files.cpp', ['name'])).toEqual([]);
            expect(names('  ')).toEqual(['saveFile', 'loadFile', 'ping']);
        });

        it('tell WebFront\'s own functions from the application\'s', () => {
            expect(['webfront.developer.read', 'webfront.commands.list', 'wfDom.ready', 'wfBind.exchange'].every(ownFunction)).toBeTrue();
            expect(['save', 'webfrontier', 'app.webfront.x'].some(ownFunction)).toBeFalse();
        });

        it('write a duration and a period shortly', () => {
            expect(formatDuration(null)).toBe('–');
            expect(formatDuration(0.0425)).toBe('43 µs');
            expect(formatDuration(1.234)).toBe('1.23 ms');
            expect(formatDuration(125.4)).toBe('125 ms');
            expect(formatDuration(2500)).toBe('2.50 s');
            expect(periods.map(formatPeriod)).toEqual(['250 ms', '1 s', '5 s']);
        });
    });

    describe('periodic reads', () => {
        it('read at once, then one period after each read ends, never two at a time', async () => {
            const timers = manualTimers();
            const read = [], shown = [];
            let release = null;
            const poller = createPoller({read: () => { read.push(read.length); return new Promise(resolve => { release = resolve; }); },
                onData: data => shown.push(data), period: 250, timers});
            const started = poller.start();
            expect(read.length).toBe(1);
            expect(timers.pending).toBeFalse();  // nothing is scheduled while a read is in progress
            release('first');
            await started;
            expect(shown).toEqual(['first']);
            expect(timers.delays).toEqual([250]);
            timers.fire();
            expect(read.length).toBe(2);
            release('second');
            await poller.settled();
            expect(shown).toEqual(['first', 'second']);
            poller.stop();
        });

        it('follow the period chosen, from the wait in progress', async () => {
            const timers = manualTimers();
            const poller = createPoller({read: async () => 'data', onData: () => {}, period: 250, timers});
            await poller.start();
            expect(timers.delays).toEqual([250]);
            poller.setPeriod(5000);
            expect(poller.period).toBe(5000);
            expect(timers.delays).toEqual([250, 5000]);
            timers.fire();
            await poller.settled();
            expect(timers.delays).toEqual([250, 5000, 5000]);
            poller.stop();
        });

        it('stop asking while paused, show nothing read after the pause, and ask again once resumed', async () => {
            const timers = manualTimers();
            const shown = [], seen = [];
            let release = null;
            const poller = createPoller({read: ({active}) => new Promise(resolve => { release = () => { seen.push(active()); resolve('late'); }; }),
                onData: data => shown.push(data), period: 250, timers});
            const started = poller.start();
            poller.pause();
            release();
            await started;
            expect(seen).toEqual([false]);
            expect(shown).toEqual([]);
            expect(timers.pending).toBeFalse();
            poller.resume();
            expect(timers.pending).toBeTrue();
            poller.stop();
            expect(timers.pending).toBeFalse();
        });

        it('report a failed read and keep reading', async () => {
            const timers = manualTimers();
            const errors = [], shown = [];
            let fail = true;
            const poller = createPoller({read: async () => { if (fail) throw new Error('lost'); return 'back'; },
                onData: data => shown.push(data), onError: error => errors.push(error.message), period: 250, timers});
            await poller.start();
            expect(errors).toEqual(['lost']);
            fail = false;
            timers.fire();
            await poller.settled();
            expect(shown).toEqual(['back']);
            poller.stop();
        });

        it('say why a page that is not admitted is refused, and when the library is too old', () => {
            expect(readFailure({code: 'webfront.inspector.denied'}, 'The functions')).toContain('not admitted');
            expect(readFailure({code: 'webfront.notFound'}, 'The functions')).toContain('update it');
            expect(readFailure(new Error('closed'), 'The functions')).toBe('The functions cannot be read: closed');
        });

        it('keep the period a viewer chose, and fall back on the default', () => {
            expect(keptPeriod({localStorage: {getItem: () => '1000'}})).toBe(1000);
            expect(keptPeriod({localStorage: {getItem: () => '123'}})).toBe(250);
            expect(keptPeriod({localStorage: {getItem: () => { throw new Error('blocked'); }}})).toBe(250);
            expect(keptPeriod({})).toBe(250);
        });
    });

    describe('table', () => {
        const rows = () => [1, 2, 3].map(id => readFunctionRow(functionTuple(id, ['save', 'load', 'ping'][id - 1], {calls: [5, 50, 0][id - 1]})));

        it('sorts by the header clicked, largest first for a number, and reverses on a second click', () => {
            const sorts = [];
            const table = createTable(document, {columns: functionColumns, caption: 'Functions', onSort: sort => sorts.push(sort)});
            table.render(rows());
            expect(column(table, 0)).toEqual(['load', 'ping', 'save']);
            const header = label => [...table.element.querySelectorAll('th')].find(th => th.textContent === label);
            expect(header('Name').getAttribute('aria-sort')).toBe('ascending');
            header('Calls').querySelector('button').click();
            expect(column(table, 0)).toEqual(['load', 'save', 'ping']);
            expect(header('Calls').getAttribute('aria-sort')).toBe('descending');
            expect(header('Name').getAttribute('aria-sort')).toBe('none');
            header('Calls').querySelector('button').click();
            expect(column(table, 0)).toEqual(['ping', 'save', 'load']);
            expect(sorts).toEqual([{sort: 'calls', descending: true}, {sort: 'calls', descending: false}]);
        });

        it('updates the cells that changed and keeps the rows that stay', () => {
            const table = createTable(document, {columns: functionColumns, caption: 'Functions'});
            table.render(rows());
            const [load, ping, save] = table.rows;
            const updated = rows();
            updated[0] = readFunctionRow(functionTuple(1, 'save', {calls: 6, failures: 1, lastError: 'disk full', flags: 4, mean: 0.5, longest: 2}));
            updated.splice(2, 1);  // ping is gone
            table.render(updated);
            expect(table.rows).toEqual([load, save]);
            expect(ping.isConnected || ping.parentNode).toBeFalsy();
            expect(cells(save).slice(2, 6)).toEqual(['6', '1', '500 µs', '2.00 ms']);
            expect(cells(save).at(-1)).toBe('disk full');
            expect(cells(load)[4]).toBe('–');
            expect(load.children[4].title).toContain('while this panel is open');
        });
    });

    describe('functions panel', () => {
        const setup = async (tuples, calls = () => [[1, 'draw', 3, 0], [2, 'webfront.commands.output', 9, 0]]) => {
            const timers = manualTimers();
            const view = panelView(timers);
            const bridge = fakeBridge({[functionsFunction]: paged(tuples, 200), [jsFunctionsFunction]: paged(calls, 200)});
            const parts = panelParts();
            const panel = await showFunctionsPanel({...parts, view, bridge});
            return {timers, view, bridge, panel, ...parts};
        };

        it('shows the application\'s functions, hides WebFront\'s own unless asked, and searches', async () => {
            const tuples = () => [functionTuple(1, 'webfront.developer.read'), functionTuple(2, 'saveFile', {calls: 4}), functionTuple(3, 'ping', {calls: 9})];
            const {panel, status, body} = await setup(tuples);
            expect(column(panel.functions, 0)).toEqual(['ping', 'saveFile']);
            expect(column(panel.jsFunctions, 0)).toEqual(['draw']);
            expect(status.textContent).toContain('2 of 2 C++ functions, 1 of WebFront\'s own hidden; 1 JavaScript functions called.');
            panel.own.checked = true;
            panel.own.dispatchEvent(new Event('change'));
            expect(column(panel.functions, 0)).toEqual(['ping', 'saveFile', 'webfront.developer.read']);
            expect(column(panel.jsFunctions, 0)).toEqual(['draw', 'webfront.commands.output']);
            panel.toolbar.search.value = 'FILE';
            panel.toolbar.search.dispatchEvent(new Event('input'));
            expect(column(panel.functions, 0)).toEqual(['saveFile']);
            expect(status.textContent).toContain('1 of 3 C++ functions');
            expect(body.querySelectorAll('table.registry').length).toBe(2);
            panel.poller.stop();
        });

        it('updates the values at the period chosen, which the viewer keeps', async () => {
            let calls = 1;
            const {panel, timers, view, bridge} = await setup(() => [functionTuple(1, 'ping', {calls})]);
            expect(column(panel.functions, 2)).toEqual(['1']);
            expect(timers.delays).toEqual([250]);
            calls = 2;
            timers.fire();
            await panel.poller.settled();
            expect(column(panel.functions, 2)).toEqual(['2']);
            panel.toolbar.period.value = '1000';
            panel.toolbar.period.dispatchEvent(new Event('change'));
            expect(timers.delays.at(-1)).toBe(1000);
            expect(view.storage.get('webfront.inspector.period')).toBe('1000');
            calls = 3;
            timers.fire();
            await panel.poller.settled();
            expect(column(panel.functions, 2)).toEqual(['3']);
            // Paused, it asks nothing more; the page that hides stops it for good.
            const asked = bridge.calls.length;
            panel.toolbar.pause.click();
            expect(panel.toolbar.pause.textContent).toBe('Resume');
            expect(timers.pending).toBeFalse();
            panel.toolbar.pause.click();
            expect(panel.toolbar.pause.textContent).toBe('Pause');
            expect(timers.pending).toBeTrue();
            view.listeners.get('pagehide')();
            expect(timers.pending).toBeFalse();
            expect(bridge.calls.length).toBe(asked);
        });

        it('says why a page that is not admitted reads nothing', async () => {
            const timers = manualTimers();
            const bridge = fakeBridge({[functionsFunction]: () => { throw Object.assign(new Error('refused'), {code: 'webfront.inspector.denied'}); }});
            const parts = panelParts();
            const panel = await showFunctionsPanel({...parts, view: panelView(timers), bridge});
            expect(parts.status.textContent).toContain('not admitted');
            expect(parts.status.classList.contains('error')).toBeTrue();
            panel.poller.stop();
        });
    });

    describe('browsers panel', () => {
        it('shows the browsers, the tasks of the scope and the commands, and searches them', async () => {
            const timers = manualTimers();
            const browsers = () => [[0, 1 | 2 | 4 | 16, '/', '', 0, 1, 2], [3, 4, '/sub/', '/s/alice/', 5, 0, 0]];
            const bridge = fakeBridge({
                [browsersFunction]: (from, count) => [browsers().filter(([id]) => id >= from).slice(0, count), 0, 7],
                [commandsFunction]: () => JSON.stringify([{name: 'deploy', help: 'ships the project'}]),
            });
            const parts = panelParts();
            const panel = await showBrowsersPanel({...parts, view: panelView(timers), bridge});
            expect(panel.summary.textContent).toBe('2 connected browsers, 7 running tasks in the server\'s scope, 1 terminal command.');
            // In the order of the identifiers; each row tells what the browser is.
            expect(panel.browsers.rows.map(cells)).toEqual([
                ['0 (this page)', 'embedded view', 'yes', 'started', '/', '–', '0', '1', '2'],
                ['3', 'external browser', 'no', 'started', '/sub/', '/s/alice/', '5', '0', '0'],
            ]);
            expect(panel.commands.rows.map(cells)).toEqual([['deploy', 'ships the project']]);
            panel.toolbar.search.value = 'alice';
            panel.toolbar.search.dispatchEvent(new Event('input'));
            expect(column(panel.browsers, 0)).toEqual(['3']);
            expect(panel.commands.rows.length).toBe(0);
            panel.poller.stop();
        });

        it('refuses a page without the number of tasks', async () => {
            const bridge = fakeBridge({[browsersFunction]: () => [[], 0]});
            const parts = panelParts();
            const panel = await showBrowsersPanel({...parts, view: panelView(manualTimers()), bridge});
            expect(parts.status.textContent).toContain('does not read');
            panel.poller.stop();
        });
    });

    describe('page', () => {
        it('shows the functions panel for inspector.html?panel=cpp.functions, with the bridge of its host in a frame', async () => {
            const frame = document.createElement('iframe');
            document.body.append(frame);
            try {
                const timers = manualTimers();
                const bridge = Object.assign(fakeBridge({[functionsFunction]: paged(() => [functionTuple(1, 'ping', {calls: 2})], 200),
                    [jsFunctionsFunction]: paged(() => [], 200)}), {ready: Promise.resolve()});
                const view = {...panelView(timers), document: frame.contentDocument, parent: {webfrontInspectorHost: {bridge}},
                    location: {href: 'http://localhost/webfront/developer/environment/inspector.html?panel=cpp.functions&frame=1'}};
                const shown = await main(view);
                expect(frame.contentDocument.title).toBe('WebFront inspector: C++ state: Functions');
                expect([...frame.contentDocument.querySelectorAll('table.registry tbody tr')].map(row => row.children[0].textContent)).toEqual(['ping']);
                expect(frame.contentDocument.querySelector('header .pause')).not.toBeNull();
                shown.poller.stop();
            } finally { frame.remove(); }
        });
    });
});
