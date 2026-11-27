// Specs of the panel of the bridge in the page (#684, ADR-027 section 8): the monitor that follows a bridge only while an
// inspector reads, the rows of a state, their publication to C++ for a detached panel, the read of what was published, and
// the panel. A real bridge is followed in test/web/suite/specs/bridge-state-specs.mjs.
import {createBridgeMonitor, createBridgePublisher, readPublishedState, toRows, fromRows, emptyState, boundedText, rejectionReason,
    ownTraffic, interfaceTarget, defaultLease, digest, showBridgePanel, bridgeSource, describeReady, formatTime, limits, readingAttempts, staleAfter,
    sourceFunction, publishFunction, bridgeFunction, publisherGlobal} from '../../../js/developer/environment/inspector-bridge.mjs';
import {ownFunction} from '../../../js/developer/environment/inspector-registry.mjs';
import {main} from '../../../js/developer/environment/inspector.mjs';

/** A bridge as WebFront.js is one for a monitor: `probe`, `pendingCalls`, the state of `ready`, and calls settled by hand. */
function fakeWebFront({state = 'linked', readySettled = true, readyError = null} = {}) {
    let nextId = 1;
    const bridge = {probe: null, readyError, readySettled, state, pendingCalls: new Map(),
        /** A call to C++ as callCppFunction makes it; `settle.resolve(value)` or `settle.reject(error)` answer it. */
        call(name) {
            const settle = {};
            const promise = new Promise((resolve, reject) => {
                if (bridge.probe) [resolve, reject] = bridge.probe.call(name, resolve, reject);
                const id = nextId++;
                bridge.pendingCalls.set(id, {resolve, reject, name});
                const answer = method => value => {
                    const pending = bridge.pendingCalls.get(id);
                    bridge.pendingCalls.delete(id);
                    pending[method](value);
                };
                Object.assign(settle, {resolve: answer('resolve'), reject: answer('reject')});
            });
            promise.catch(() => {});
            return {promise, ...settle};
        },
        /** A call of C++ to JavaScript as callJsFunction tells it. */
        called(name) { bridge.probe?.jsCall(name); },
    };
    return bridge;
}

/** A clock and timers moved by hand: `advance(ms)` runs the timers that come due. */
function manualTime() {
    let time = 0, next = 1;
    const timers = new Map();
    return {
        clock: () => time,
        now: () => 1_700_000_000_000 + time,
        get pending() { return timers.size; },
        setTimeout(run, delay) { timers.set(next, {run, at: time + delay}); return next++; },
        clearTimeout(id) { timers.delete(id); },
        advance(milliseconds) {
            const end = time + milliseconds;
            for (;;) {
                const due = [...timers].filter(([, timer]) => timer.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
                if (!due) break;
                timers.delete(due[0]);
                time = Math.max(time, due[1].at);
                due[1].run();
            }
            time = end;
        },
    };
}

const monitorOf = (bridge, time, options = {}) => createBridgeMonitor({target: () => ({bridge, label: 'interface', path: '/app/'}),
    clock: time.clock, now: time.now, timers: time, ...options});

/** A C++ relay in memory: one page per host, the lease told by `wanted`. */
function fakeRelay({wanted = true, pageSize = 200} = {}) {
    const relay = {wanted, published: new Map(), publication: 0, calls: [], sources: new Set(), age: 0,
        list: items => ({list: [...items]}),
        cppFunction: name => async (...args) => {
            relay.calls.push([name, ...args]);
            if (name === sourceFunction) { relay.sources.add(args[0]); return relay.wanted; }
            if (name === publishFunction) {
                relay.published.set(args[0], {rows: args[1].list, publication: ++relay.publication});
                return relay.wanted;
            }
            if (name === bridgeFunction) {
                const [host, from, count] = args;
                const entry = relay.published.get(host);
                relay.onRead?.(from);
                if (!entry) return [[], 0, 0, 0, relay.sources.has(host) ? 1 : 0];
                const rows = entry.rows.slice(from, from + Math.min(count, pageSize));
                return [rows, from + rows.length < entry.rows.length ? from + rows.length : 0, entry.publication, relay.age, 3];
            }
            throw Object.assign(new Error(`C++ function '${name}' was not found`), {code: 'webfront.notFound'});
        }};
    return relay;
}

const sampleState = () => ({
    page: {label: 'interface', path: '/app/'}, ready: {state: 'resolved', reason: ''}, connection: 'linked', since: 1_700_000_000_000,
    history: true, pendingTotal: 2, rejectedTotal: 1, jsTotal: 1, jsSaturated: false,
    pending: [{id: 3, name: 'app.save', elapsed: 1250}, {id: 4, name: 'app.load', elapsed: null}],
    rejections: [{id: 5, name: 'app.open', reason: 'app.missing: no such file', at: 1_700_000_001_000}],
    jsFunctions: [{id: 6, name: 'draw', calls: 12}],
});

function panelParts() {
    const header = document.createElement('header');
    header.append(document.createElement('h1'));
    const body = document.createElement('main');
    const status = document.createElement('p');
    body.append(status);
    return {page: document, header, body, status};
}

/** A view for a panel: timers run by hand, a storage and listeners of its own. */
function panelView() {
    let pending = null, next = 1;
    const listeners = new Map();
    return {
        setTimeout(run) { pending = {id: next, run}; return next++; },
        clearTimeout(id) { if (pending?.id === id) pending = null; },
        fire() { const {run} = pending; pending = null; run(); },
        localStorage: {getItem: () => null, setItem: () => {}},
        addEventListener: (name, handler) => listeners.set(name, handler),
        listeners,
    };
}

const column = (table, index) => table.rows.map(row => row.children[index].textContent);

describe('The panel of the bridge in the page', () => {
    describe('texts', () => {
        it('are cut on a character boundary within their bytes, and end with an ellipsis when cut', () => {
            expect(boundedText('short')).toBe('short');
            const cut = boundedText('é'.repeat(200));
            expect(new TextEncoder().encode(cut).length).toBeLessThanOrEqual(limits.text);
            expect(cut.endsWith('…')).toBeTrue();
            expect(cut.slice(0, -1)).toBe('é'.repeat(126));
            expect(boundedText('😀😀😀', 8)).toBe('😀…');
        });

        it('show control characters as their pictures, as C++ does', () => {
            expect(boundedText('a\nb\u0000c\u007f')).toBe('a␊b␀c␡');
        });

        it('never throw for a value that cannot be read', () => {
            expect(boundedText({toString() { throw new Error('no'); }})).toBe('(a value that cannot be read)');
            expect(rejectionReason({get message() { throw new Error('no'); }, toString() { throw new Error('no'); }})).toBe('A rejection whose reason cannot be read');
        });

        it('give the reason of a rejection: the code of a coded error, then its message', () => {
            expect(rejectionReason(Object.assign(new Error('the user said no'), {code: 'app.denied'}))).toBe('app.denied: the user said no');
            expect(rejectionReason(Object.assign(new Error('C++ callback failed'), {code: 'webfront.error'}))).toBe('C++ callback failed');
            expect(rejectionReason(new Error('plain'))).toBe('plain');
            expect(rejectionReason('a string')).toBe('a string');
            expect(rejectionReason(undefined)).toBe('undefined');
        });

        it('read the message of an error of another frame, which is not an Error of this page', () => {
            const frame = document.createElement('iframe');
            document.body.append(frame);
            try {
                const foreign = Object.assign(new frame.contentWindow.Error('the user said no'), {code: 'denied'});
                expect(foreign instanceof Error).toBeFalse();
                expect(rejectionReason(foreign)).toBe('denied: the user said no');
                expect(rejectionReason(new frame.contentWindow.TypeError('not a wire value'))).toBe('not a wire value');
            } finally { frame.remove(); }
        });

        it('tell the inspectors\' own traffic, which the registry panel hides too', () => {
            expect(ownTraffic('webfront.inspector.functions')).toBeTrue();
            expect(ownTraffic(`${publisherGlobal}.publish`)).toBeTrue();
            expect(ownTraffic('webfront.developer.read')).toBeFalse();
            expect(ownTraffic('app.save')).toBeFalse();
            expect(ownFunction(`${publisherGlobal}.publish`)).toBeTrue();
        });
    });

    describe('monitor', () => {
        it('installs no probe until it is read, and removes it at the end of its lease', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time, {lease: 5000});
            expect(bridge.probe).toBeNull();
            expect(monitor.observing).toBeFalse();
            monitor.read();
            expect(bridge.probe).not.toBeNull();
            expect(monitor.observing).toBeTrue();
            time.advance(4000);
            monitor.read();  // a read renews the lease
            time.advance(4000);
            expect(bridge.probe).not.toBeNull();
            time.advance(1001);
            expect(bridge.probe).toBeNull();
            expect(time.pending).toBe(0);
            monitor.read();
            expect(bridge.probe).not.toBeNull();
        });

        it('keeps its probe longer than the slowest period of a panel, so that a panel that reads never misses a call', () => {
            expect(defaultLease).toBeGreaterThan(5000 * 2);
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            for (let read = 0; read < 4; ++read) {
                monitor.read();
                time.advance(5000 + 500);  // the period, and the time a slow read takes
                expect(bridge.probe).withContext(`after read ${read}`).not.toBeNull();
            }
        });

        it('lets go of a bridge it no longer observes, and finds its state again when it observes it anew', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            const call = bridge.call('app.slow');
            monitor.release();
            expect(monitor.observing).toBeFalse();
            call.reject(new Error('while released'));
            await expectAsync(call.promise).toBeRejected();
            const state = monitor.read();
            expect(state.rejections.map(entry => entry.name)).toEqual(['app.slow']);
        });

        it('never removes a probe that is not its own', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            const other = {call: (name, resolve, reject) => [resolve, reject], jsCall() {}};
            bridge.probe = other;
            monitor.release();
            expect(bridge.probe).toBe(other);
        });

        it('tells the state of ready and of the connection', () => {
            const time = manualTime();
            expect(monitorOf(fakeWebFront({state: 'handshaking', readySettled: false}), time).read().ready).toEqual({state: 'pending', reason: ''});
            const resolved = monitorOf(fakeWebFront(), time).read();
            expect(resolved.ready).toEqual({state: 'resolved', reason: ''});
            expect(resolved.connection).toBe('linked');
            expect(resolved.page).toEqual({label: 'interface', path: '/app/'});
            const failure = Object.assign(new Error('WebFront connection closed: connection lost'), {code: 'webfront.disconnected'});
            const rejected = monitorOf(fakeWebFront({state: 'closed', readyError: failure}), time).read();
            expect(rejected.ready).toEqual({state: 'rejected', reason: 'webfront.disconnected: WebFront connection closed: connection lost'});
            expect(rejected.connection).toBe('closed');
        });

        it('shows a call while it is pending, with how long, then no longer once it is answered', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            const call = bridge.call('app.save');
            time.advance(120);
            const during = monitor.read();
            expect(during.pending).toEqual([jasmine.objectContaining({name: 'app.save', elapsed: 120})]);
            expect(during.pendingTotal).toBe(1);
            call.resolve('saved');
            expect(await call.promise).toBe('saved');
            const after = monitor.read();
            expect(after.pending).toEqual([]);
            expect(after.rejectedTotal).toBe(0);
        });

        it('shows a rejected Promise with its function, its reason and its time, and keeps the latest only', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            time.advance(10);
            const call = bridge.call('app.open');
            call.reject(Object.assign(new Error('no such file'), {code: 'app.missing'}));
            await expectAsync(call.promise).toBeRejected();
            const state = monitor.read();
            expect(state.rejections).toEqual([jasmine.objectContaining({name: 'app.open', reason: 'app.missing: no such file', at: time.now()})]);
            expect(state.pending).toEqual([]);
            for (let index = 0; index < limits.rejections + 5; ++index) bridge.call(`call${index}`).reject(new Error('failed'));
            const many = monitor.read();
            expect(many.rejectedTotal).toBe(limits.rejections + 6);
            expect(many.rejections.length).toBe(limits.rejections);
            expect(many.rejections.at(-1).name).toBe(`call${limits.rejections + 4}`);
        });

        it('adopts the calls made before it read, without a duration, and follows them to their end', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const early = bridge.call('app.early'), failing = bridge.call('app.failing');
            const monitor = monitorOf(bridge, time);
            time.advance(50);
            expect(monitor.read().pending).toEqual([jasmine.objectContaining({name: 'app.early', elapsed: null}),
                jasmine.objectContaining({name: 'app.failing', elapsed: null})]);
            early.resolve(1);
            failing.reject(new Error('late failure'));
            await early.promise;
            const state = monitor.read();
            expect(state.pending).toEqual([]);
            expect(state.rejections.map(({name, reason}) => [name, reason])).toEqual([['app.failing', 'late failure']]);
        });

        it('still follows to its end a call made while it observed, after its probe is gone', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time, {lease: 1000});
            monitor.read();
            const call = bridge.call('app.slow');
            time.advance(2000);
            expect(bridge.probe).toBeNull();
            const unseen = bridge.call('app.unseen');
            call.reject(new Error('too late'));
            unseen.reject(new Error('never seen'));
            const state = monitor.read();
            expect(state.rejections.map(rejection => rejection.name)).toEqual(['app.slow']);
        });

        it('adopts, when it reads again, the calls made after its probe left, and follows once those it already followed', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time, {lease: 1000});
            monitor.read();
            const followedCall = bridge.call('app.followed');
            time.advance(2000);
            expect(bridge.probe).toBeNull();
            const between = bridge.call('app.between');
            const state = monitor.read();
            expect(state.pending.map(({name, elapsed}) => [name, elapsed])).toEqual([['app.followed', 2000], ['app.between', null]]);
            expect(state.pendingTotal).toBe(2);
            between.reject(new Error('seen after all'));
            followedCall.resolve();
            await followedCall.promise;
            const after = monitor.read();
            expect(after.pending).toEqual([]);
            expect(after.rejections.map(({name, reason}) => [name, reason])).toEqual([['app.between', 'seen after all']]);
            expect(after.rejectedTotal).toBe(1);
        });

        it('shows the oldest pending calls within its bound, and counts them all', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            for (let index = 0; index < limits.pending + 7; ++index) bridge.call(`call${index}`);
            const state = monitor.read();
            expect(state.pending.length).toBe(limits.pending);
            expect(state.pending[0].name).toBe('call0');
            expect(state.pendingTotal).toBe(limits.pending + 7);
        });

        it('counts the JavaScript functions that C++ calls, the most called first, within its bounds', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            bridge.called('before');
            monitor.read();
            bridge.called('draw');
            bridge.called('resize');
            bridge.called('draw');
            const state = monitor.read();
            expect(state.jsFunctions.map(({name, calls}) => [name, calls])).toEqual([['draw', 2], ['resize', 1]]);
            for (let index = 0; index < limits.jsNames + 3; ++index) bridge.called(`generated${index}`);
            const many = monitor.read();
            expect(many.jsFunctions.length).toBe(limits.jsFunctions);
            expect(many.jsFunctions[0].name).toBe('draw');
            // Past the names it keeps, the others are counted together as one more, and the count is no longer exact.
            expect(many.jsTotal).toBe(limits.jsNames + 1);
            expect(many.jsSaturated).toBeTrue();
            expect(state.jsSaturated).toBeFalse();
            expect(fromRows(toRows(many)).jsSaturated).toBeTrue();
        });

        it('tells apart two long names that share their label', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            const shared = `app.${'x'.repeat(300)}`;
            bridge.called(`${shared}.first`);
            bridge.called(`${shared}.second`);
            bridge.called(`${shared}.first`);
            const state = monitor.read();
            expect(state.jsFunctions.map(entry => entry.calls)).toEqual([2, 1]);
            expect(state.jsTotal).toBe(2);
            expect(digest(`${shared}.first`)).not.toBe(digest(`${shared}.second`));
            expect(digest('a')).toMatch(/^[0-9a-f]{16}$/);
            expect(digest('')).toBe('cbf29ce484222325');  // the FNV-1a offset basis
            expect(digest('a')).toBe('af63dc4c8601ec8c');  // FNV-1a 64 of "a"
        });

        it('keeps of a long name no more than its bounded label', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            const long = name => `${name}.${'x'.repeat(100_000)}`;
            bridge.called(long('a'));
            bridge.called(long('a'));
            bridge.called(long('b'));
            const state = monitor.read();
            expect(state.jsFunctions.map(entry => entry.calls)).toEqual([2, 1]);
            expect(state.jsFunctions.every(entry => new TextEncoder().encode(entry.name).length <= limits.text)).toBeTrue();
        });

        it('does not show the inspectors\' own traffic', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const earlier = bridge.call('webfront.inspector.functions');
            const monitor = monitorOf(bridge, time);
            monitor.read();
            bridge.call('webfront.inspector.publishBridge').reject(new Error('denied'));
            bridge.called('wfInspect.publish');
            earlier.reject(new Error('denied'));
            const state = monitor.read();
            expect(state.pending).toEqual([]);
            expect(state.rejections).toEqual([]);
            expect(state.jsFunctions).toEqual([]);
        });

        it('never fails a call of the application, whatever the name or the reason', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            const hostile = {toString() { throw new Error('no name'); }};
            const call = bridge.call(hostile);
            call.resolve('answered');
            expect(await call.promise).toBe('answered');
            expect(() => bridge.called(hostile)).not.toThrow();
            const rejected = bridge.call('app.fails');
            const reason = {get message() { throw new Error('no'); }, toString() { throw new Error('no'); }};
            rejected.reject(reason);
            await expectAsync(rejected.promise).toBeRejectedWith(reason);
            expect(monitor.read().rejections.at(-1).reason).toBe('A rejection whose reason cannot be read');
        });

        it('settles every call of the application even when the monitor fails, also as the bridge closes', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            let broken = false;
            const monitor = monitorOf(bridge, time, {now: () => { if (broken) throw new Error('clock gone'); return time.now(); }});
            monitor.read();
            const calls = [bridge.call('app.one'), bridge.call('app.two'), bridge.call('app.three')];
            broken = true;
            // As WebFront.js does on close: reject every pending call in turn, then clear them.
            const closed = Object.assign(new Error('WebFront connection closed'), {code: 'webfront.disconnected'});
            for (const pending of [...bridge.pendingCalls.values()]) expect(() => pending.reject(closed)).not.toThrow();
            for (const call of calls) await expectAsync(call.promise).toBeRejectedWith(closed);
            const answered = bridge.call('app.four');
            answered.resolve('kept');
            expect(await answered.promise).toBe('kept');
        });

        it('finds the pending calls of a bridge that predates the probe at each read, and learns ready from its Promise', async () => {
            let rejectReady;
            const ready = new Promise((resolve, reject) => { rejectReady = reject; });
            ready.catch(() => {});
            // A WebFront.js before #684: no probe, no readyError, pending calls without their name.
            const old = {readySettled: false, state: 'handshaking', ready, pendingCalls: new Map()};
            const time = manualTime();
            const monitor = monitorOf(old, time);
            const first = monitor.read();
            expect('probe' in old).withContext('no probe is set on a bridge that would not read it').toBeFalse();
            expect(monitor.observing).toBeFalse();
            expect(first.history).toBeFalse();
            expect(first.ready.state).toBe('pending');
            let settle;
            const promise = new Promise((resolve, reject) => { settle = {resolve, reject}; });
            promise.catch(() => {});
            old.pendingCalls.set(1, {...settle});
            expect(monitor.read().pending.map(call => call.name)).toEqual(['(unknown)']);
            expect(monitor.read().pending.length).withContext('found once, not at each read').toBe(1);
            old.pendingCalls.get(1).reject(new Error('refused'));
            old.pendingCalls.delete(1);
            await expectAsync(promise).toBeRejected();
            const after = monitor.read();
            expect(after.pending).toEqual([]);
            expect(after.rejections.map(rejection => rejection.reason)).toEqual(['refused']);
            // ready was rejected: an old bridge is not shown "resolved".
            old.readySettled = true;
            old.state = 'closed';
            expect(monitor.read().ready.state).toBe('unknown');
            rejectReady(new Error('WebFront connection failed'));
            await Promise.resolve();
            await Promise.resolve();
            expect(monitor.read().ready).toEqual({state: 'rejected', reason: 'WebFront connection failed'});
            expect(fromRows(toRows(monitor.read())).history).toBeFalse();
        });

        it('starts from a new state when the page of the interface is loaded again', () => {
            const first = fakeWebFront(), second = fakeWebFront(), time = manualTime();
            let current = first;
            const monitor = createBridgeMonitor({target: () => ({bridge: current, label: 'interface', path: '/app/'}), clock: time.clock, now: time.now, timers: time});
            monitor.read();
            first.call('app.old');
            expect(monitor.read().pendingTotal).toBe(1);
            current = second;
            const state = monitor.read();
            expect(state.pendingTotal).toBe(0);
            expect(first.probe).toBeNull();
            expect(second.probe).not.toBeNull();
        });

        it('wraps a pending call once, however many times the target comes back to its bridge', async () => {
            const page = fakeWebFront(), time = manualTime();
            let current = page;
            const monitor = createBridgeMonitor({target: () => ({bridge: current, label: 'this page', path: '/'}), clock: time.clock, now: time.now, timers: time});
            monitor.read();
            const call = page.call('app.long');
            const wrapped = [...page.pendingCalls.values()][0].resolve;
            for (let reload = 0; reload < 3; ++reload) {
                current = fakeWebFront();  // the interface's frame has a bridge
                monitor.read();
                current = page;            // the frame is loaded again: back to the page's own bridge
                expect(monitor.read().pending.map(entry => entry.name)).toEqual(['app.long']);
            }
            expect([...page.pendingCalls.values()][0].resolve).withContext('the call keeps the wrapper it had').toBe(wrapped);
            call.reject(new Error('failed once'));
            await expectAsync(call.promise).toBeRejected();
            const state = monitor.read();
            expect(state.rejectedTotal).toBe(1);
            expect(state.pending).toEqual([]);
        });

        it('gives the state of a page without a bridge, and of a target that cannot be read', () => {
            const time = manualTime();
            expect(createBridgeMonitor({target: () => null, clock: time.clock, now: time.now, timers: time}).read()).toEqual(emptyState());
            const crossOrigin = createBridgeMonitor({target: () => { throw new DOMException('blocked', 'SecurityError'); }, clock: time.clock, now: time.now, timers: time});
            expect(crossOrigin.read().ready.state).toBe('none');
        });

        it('follows the interface\'s bridge when its frame has one, else leaves the page\'s own', () => {
            const bridge = fakeWebFront();
            expect(interfaceTarget({contentWindow: {webFront: bridge, location: {pathname: '/webfront/developer/preview/index.html'}}}))
                .toEqual({bridge, label: 'interface', path: '/webfront/developer/preview/index.html'});
            expect(interfaceTarget({contentWindow: {location: {pathname: '/'}}})).toBeNull();
            expect(interfaceTarget({contentWindow: null})).toBeNull();
            expect(interfaceTarget(null)).toBeNull();
        });
    });

    describe('rows', () => {
        it('carry a state both ways, each of one shape: three texts and two numbers', () => {
            const rows = toRows(sampleState());
            expect(rows.every(row => row.length === 5 && row.slice(0, 3).every(text => typeof text === 'string')
                && row.slice(3).every(Number.isFinite))).toBeTrue();
            expect(fromRows(rows)).toEqual(sampleState());
            expect(fromRows(toRows(emptyState()))).toEqual(emptyState());
        });

        it('of the fullest state fit what C++ keeps for a page', () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            monitor.read();
            for (let index = 0; index < 300; ++index) {
                bridge.call(`pending${index}`);
                bridge.call(`rejected${index}`).reject(new Error('x'));
                bridge.called(`js${index}`);
            }
            expect(toRows(monitor.read()).length).toBeLessThanOrEqual(256);
        });

        it('ignore a kind they do not know, and refuse what is not a row', () => {
            expect(fromRows([['later', 'x', '', 0, 0], ['connection', 'linked', '', 0, 0]]).connection).toBe('linked');
            expect(() => fromRows([['pending', 'x', '', 0]])).toThrowError(/does not read/);
            expect(() => fromRows([['pending', 'x', 3, 0, 0]])).toThrowError(/does not read/);
            expect(() => fromRows('rows')).toThrowError(/does not read/);
        });
    });

    describe('publisher', () => {
        const setup = (relayOptions = {}) => {
            const bridge = fakeWebFront(), time = manualTime(), relay = fakeRelay(relayOptions);
            const view = {setTimeout: (run, delay) => time.setTimeout(run, delay), clearTimeout: id => time.clearTimeout(id)};
            const monitor = monitorOf(bridge, time);
            const publisher = createBridgePublisher({monitor, bridge: relay, host: 'host-a', view, period: 250});
            return {bridge, time, relay, view, monitor, publisher};
        };
        const settled = async () => { for (let turn = 0; turn < 5; ++turn) await Promise.resolve(); };

        it('announces the page and publishes nothing while no panel reads', async () => {
            const {relay, publisher, bridge, view} = setup({wanted: false});
            await publisher.announce();
            expect(relay.calls).toEqual([[sourceFunction, 'host-a']]);
            expect(publisher.running).toBeFalse();
            expect(bridge.probe).withContext('nothing is recorded in the bridge').toBeNull();
            expect(typeof view[publisherGlobal].publish).toBe('function');
        });

        it('publishes at its period once C++ asks, and stops when told that no panel reads any more', async () => {
            const {relay, publisher, bridge, view, time} = setup({wanted: false});
            await publisher.announce();
            relay.wanted = true;
            view[publisherGlobal].publish();
            view[publisherGlobal].publish();  // a second request while it publishes starts nothing more
            await settled();
            expect(relay.published.get('host-a').publication).toBe(1);
            expect(relay.published.get('host-a').rows).toEqual(toRows(fromRows(relay.published.get('host-a').rows)));
            expect(bridge.probe).not.toBeNull();
            bridge.call('app.save');
            time.advance(250);
            await settled();
            expect(relay.published.get('host-a').publication).toBe(2);
            expect(fromRows(relay.published.get('host-a').rows).pending.map(call => call.name)).toEqual(['app.save']);
            relay.wanted = false;
            time.advance(250);
            await settled();
            expect(relay.published.get('host-a').publication).toBe(3);
            expect(publisher.running).toBeFalse();
            time.advance(60000);
            await settled();
            expect(relay.published.get('host-a').publication).withContext('no publication without a reader').toBe(3);
            expect(bridge.probe).withContext('the probe left the bridge with its lease').toBeNull();
        });

        it('starts at once when a panel already waits for the page that announces itself', async () => {
            const {relay, publisher} = setup({wanted: true});
            await publisher.announce();
            await settled();
            expect(relay.published.get('host-a').publication).toBe(1);
            expect(publisher.running).toBeTrue();
            publisher.stop();
        });

        it('announces itself again when the relay forgot the page, and goes on publishing if a panel waits', async () => {
            const {relay, publisher} = setup({wanted: true});
            const answer = relay.cppFunction;
            let forgotten = true;
            relay.cppFunction = name => async (...args) => {
                if (name === publishFunction && forgotten) {
                    relay.calls.push([name, args[0]]);
                    throw Object.assign(new Error('This page did not announce the state of its bridge'), {code: 'webfront.inspector.unknown'});
                }
                if (name === sourceFunction) forgotten = false;
                return answer(name)(...args);
            };
            publisher.start();
            await settled();
            await settled();
            expect(relay.calls.map(call => call[0])).toEqual([publishFunction, sourceFunction, publishFunction]);
            expect(relay.published.get('host-a').publication).toBe(1);
            expect(publisher.running).toBeTrue();
            publisher.stop();
        });

        it('stays silent with a library that has no relay, and stops on a failed publication', async () => {
            const {publisher, bridge} = setup();
            const calls = [];
            const failing = createBridgePublisher({monitor: monitorOf(bridge, manualTime()), host: 'host-a', view: {setTimeout() {}, clearTimeout() {}},
                bridge: {list: items => items, cppFunction: name => async () => { calls.push(name); throw Object.assign(new Error('not found'), {code: 'webfront.notFound'}); }}});
            await failing.announce();
            expect(calls).toEqual([sourceFunction]);
            expect(failing.running).toBeFalse();
            failing.start();
            await settled();
            expect(calls).toEqual([sourceFunction, publishFunction]);
            expect(failing.running).toBeFalse();
            publisher.stop();
        });
    });

    describe('published state', () => {
        it('is read page after page', async () => {
            const relay = fakeRelay({pageSize: 3});
            relay.sources.add('host-a');
            relay.published.set('host-a', {rows: toRows(sampleState()), publication: 7});
            relay.age = 40;
            const read = await readPublishedState(relay, 'host-a');
            expect(read).toEqual({state: sampleState(), age: 40, source: true, published: true});
            expect(relay.calls.map(call => call[2])).toEqual([0, 3, 6]);
        });

        it('starts again when the page published between two pages, and gives up after too many', async () => {
            const relay = fakeRelay({pageSize: 3});
            relay.published.set('host-a', {rows: toRows(sampleState()), publication: 1});
            let changes = 1;
            relay.onRead = from => { if (from === 3 && changes-- > 0) relay.published.get('host-a').publication = 2; };
            expect((await readPublishedState(relay, 'host-a')).state).toEqual(sampleState());
            relay.onRead = from => { if (from === 3) ++relay.published.get('host-a').publication; };
            relay.calls.length = 0;
            await expectAsync(readPublishedState(relay, 'host-a')).toBeRejectedWithError(/during each of 8 reads/);
            expect(relay.calls.length).toBe(2 * readingAttempts);
        });

        it('tells a page that has not published yet, and one that is not connected', async () => {
            const relay = fakeRelay();
            expect(await readPublishedState(relay, 'gone')).toEqual({state: null, age: 0, source: false, published: false});
            relay.sources.add('host-a');
            expect(await readPublishedState(relay, 'host-a')).toEqual({state: null, age: 0, source: true, published: false});
        });

        it('stops once it is no longer active, and refuses an answer that is not a page', async () => {
            const relay = fakeRelay();
            expect(await readPublishedState(relay, 'host-a', {active: () => false})).toBeNull();
            expect(relay.calls).toEqual([]);
            const wrong = {cppFunction: () => async () => [[], 0, 0]};
            await expectAsync(readPublishedState(wrong, 'host-a')).toBeRejectedWithError(/does not read/);
            const stuck = {cppFunction: () => async () => [[['js', 'draw', '', 1, 1]], 0.5, 1, 0, 3]};
            await expectAsync(readPublishedState(stuck, 'host-a')).toBeRejectedWithError(/does not read/);
        });
    });

    describe('panel', () => {
        const setup = async source => {
            const view = panelView(), parts = panelParts();
            const panel = await showBridgePanel({...parts, view, source});
            return {view, panel, ...parts};
        };
        const read = (state, extra = {}) => ({state, age: 0, source: true, published: true, ...extra});

        it('shows ready, the connection, the pending calls, the rejections and the JavaScript functions', async () => {
            const {panel, status} = await setup(async () => read(sampleState()));
            expect(panel.fields.page.textContent).toBe('interface (/app/)');
            expect(panel.fields.ready.textContent).toBe('resolved');
            expect(panel.fields.connection.textContent).toBe('linked');
            expect(panel.fields.counts.textContent).toContain('2 pending, 1 rejected since ');
            expect(column(panel.pending, 0)).toEqual(['app.save', 'app.load']);
            expect(column(panel.pending, 1)).toEqual(['1.25 s', '–']);
            expect(column(panel.rejections, 1)).toEqual(['app.open']);
            expect(column(panel.rejections, 2)).toEqual(['app.missing: no such file']);
            expect(column(panel.rejections, 0)).toEqual([formatTime(1_700_000_001_000)]);
            expect(column(panel.called, 0)).toEqual(['draw']);
            expect(column(panel.called, 1)).toEqual(['12']);
            expect(status.textContent).toBe('');
        });

        it('follows a call from pending to answered at the next read, and a rejection with its reason', async () => {
            let state = sampleState();
            const {panel, view} = await setup(async () => read(state));
            expect(column(panel.pending, 0)).toContain('app.save');
            state = {...sampleState(), pendingTotal: 0, pending: [], rejectedTotal: 2,
                rejections: [...sampleState().rejections, {id: 9, name: 'app.save', reason: 'disk full', at: 1_700_000_002_000}]};
            view.fire();
            await panel.poller.settled();
            expect(column(panel.pending, 0)).toEqual([]);
            expect(column(panel.rejections, 1)).toEqual(['app.save', 'app.open']);
            expect(column(panel.rejections, 2)[0]).toBe('disk full');
        });

        it('says why ready was rejected', async () => {
            const {panel} = await setup(async () => read({...sampleState(), connection: 'closed',
                ready: {state: 'rejected', reason: 'webfront.disconnected: WebFront connection closed: connection lost'}}));
            expect(panel.fields.ready.textContent).toBe('rejected: webfront.disconnected: WebFront connection closed: connection lost');
            expect(panel.fields.ready.classList.contains('rejected')).toBeTrue();
            expect(describeReady({state: 'pending', reason: ''})).toContain('pending');
            expect(describeReady({state: 'none', reason: ''})).toContain('no WebFront bridge');
        });

        it('shows the texts of the page as text, never as markup', async () => {
            const hostile = '<img src=x onerror="window.injected = true">';
            const {panel, body} = await setup(async () => read({...sampleState(), page: {label: hostile, path: hostile},
                pending: [{id: 1, name: hostile, elapsed: 1}], rejections: [{id: 2, name: hostile, reason: hostile, at: 0}],
                jsFunctions: [{id: 3, name: hostile, calls: 1}]}));
            expect(body.querySelector('img')).toBeNull();
            expect(column(panel.pending, 0)).toEqual([hostile]);
            expect(column(panel.rejections, 2)).toEqual([hostile]);
            expect(window.injected).toBeUndefined();
        });

        it('searches the calls by name and by reason', async () => {
            const {panel, toolbar = panel.toolbar} = await setup(async () => read(sampleState()));
            toolbar.search.value = 'missing';
            toolbar.search.dispatchEvent(new Event('input'));
            expect(column(panel.pending, 0)).toEqual([]);
            expect(column(panel.rejections, 1)).toEqual(['app.open']);
            toolbar.search.value = 'SAVE';
            toolbar.search.dispatchEvent(new Event('input'));
            expect(column(panel.pending, 0)).toEqual(['app.save']);
        });

        it('says what is not shown, and that a published state is old', async () => {
            const {status} = await setup(async () => read({...sampleState(), pendingTotal: 130, jsTotal: 105}, {age: staleAfter + 2000}));
            expect(status.textContent).toContain('The page published this state 5.00 s ago.');
            expect(status.textContent).toContain('128 more pending calls');
            expect(status.textContent).toContain('104 more JavaScript functions');
            expect(status.textContent).not.toContain('at least');
        });

        it('says when the page\'s WebFront.js predates the inspectors', async () => {
            const {status} = await setup(async () => read({...sampleState(), history: false}));
            expect(status.textContent).toContain('predates the inspectors');
            expect(describeReady({state: 'unknown', reason: ''})).toContain('not known yet');
        });

        it('says "at least" for the JavaScript functions it could not count by name', async () => {
            const {status} = await setup(async () => read({...sampleState(), jsTotal: 1001, jsSaturated: true}));
            expect(status.textContent).toContain('at least 1000 more JavaScript functions');
        });

        it('waits for a page that has not published yet, and says when that page is not connected', async () => {
            let answer = {state: null, age: 0, source: true, published: false};
            const {panel, status, view} = await setup(async () => answer);
            expect(status.textContent).toContain('Waiting for the page to publish');
            answer = read(sampleState());
            view.fire();
            await panel.poller.settled();
            expect(status.textContent).toBe('');
            expect(column(panel.pending, 0)).toContain('app.save');
            answer = {state: null, age: 0, source: false, published: false};
            view.fire();
            await panel.poller.settled();
            expect(status.textContent).toContain('is not connected');
            // What was read last stays shown, and searching it does not hide that the page is gone.
            expect(column(panel.pending, 0)).toContain('app.save');
            panel.toolbar.search.value = 'save';
            panel.toolbar.search.dispatchEvent(new Event('input'));
            expect(column(panel.pending, 0)).toEqual(['app.save']);
            expect(status.textContent).toContain('is not connected');
            // The page comes back and has not published yet: the status says so.
            answer = {state: null, age: 0, source: true, published: false};
            view.fire();
            await panel.poller.settled();
            expect(status.textContent).toContain('Waiting for the page to publish');
        });

        it('says when the published state could not be read, keeps the rows, and clears it at the next read', async () => {
            let busy = false;
            const {panel, status, view} = await setup(async () => {
                if (busy) throw new Error('the page published a new state during each of 8 reads; trying again at the next period');
                return read(sampleState());
            });
            busy = true;
            view.fire();
            await panel.poller.settled();
            expect(status.classList.contains('error')).toBeTrue();
            expect(status.textContent).toContain('during each of 8 reads');
            expect(column(panel.pending, 0)).toContain('app.save');
            busy = false;
            view.fire();
            await panel.poller.settled();
            expect(status.classList.contains('error')).toBeFalse();
        });

        it('says why a read failed, and clears it when a read succeeds', async () => {
            let fail = Object.assign(new Error('denied'), {code: 'webfront.inspector.denied'});
            const {panel, status, view} = await setup(async () => { if (fail) throw fail; return read(sampleState()); });
            expect(status.classList.contains('error')).toBeTrue();
            expect(status.textContent).toContain('not admitted');
            fail = null;
            view.fire();
            await panel.poller.settled();
            expect(status.classList.contains('error')).toBeFalse();
            expect(panel.fields.ready.textContent).toBe('resolved');
        });

        it('pauses and resumes its reads', async () => {
            let reads = 0;
            const {panel, view} = await setup(async () => { ++reads; return read(sampleState()); });
            panel.toolbar.pause.click();
            expect(panel.poller.paused).toBeTrue();
            expect(panel.toolbar.pause.textContent).toBe('Resume');
            const before = reads;
            panel.toolbar.pause.click();
            view.fire();
            await panel.poller.settled();
            expect(reads).toBe(before + 1);
        });
    });

    describe('source of a panel', () => {
        it('in a frame is the monitor of the page that holds it, through the same rows as the relay', async () => {
            const bridge = fakeWebFront(), time = manualTime();
            const monitor = monitorOf(bridge, time);
            const view = {parent: {webfrontInspectorHost: {bridgeState: monitor}}};
            const source = bridgeSource({view, bridge: null, framed: true, host: 'host-a'});
            const call = bridge.call('app.before');
            const first = await source({active: () => true});
            expect(first).toEqual({state: jasmine.objectContaining({connection: 'linked'}), age: 0, source: true, published: true});
            expect(first.state.pending.map(entry => entry.name)).toEqual(['app.before']);
            call.resolve();
            // Attached and detached show the same state: what the page would publish, read back.
            const relay = fakeRelay();
            const state = monitor.read();
            await relay.cppFunction(publishFunction)('host-a', relay.list(toRows(state)));
            expect((await readPublishedState(relay, 'host-a')).state).toEqual((await source({active: () => true})).state);
        });

        it('in a frame whose page does not follow its bridge says so', async () => {
            const source = bridgeSource({view: {parent: {webfrontInspectorHost: {}}}, bridge: null, framed: true, host: null});
            await expectAsync(source({active: () => true})).toBeRejectedWithError(/does not follow its bridge/);
        });

        it('detached is what the page named by the window published', async () => {
            const relay = fakeRelay();
            relay.published.set('host-a', {rows: toRows(sampleState()), publication: 1});
            const source = bridgeSource({view: {}, bridge: relay, framed: false, host: 'host-a'});
            expect((await source({active: () => true})).state).toEqual(sampleState());
            expect(relay.calls[0]).toEqual([bridgeFunction, 'host-a', 0, 200]);
            expect(() => bridgeSource({view: {}, bridge: relay, framed: false, host: null})({active: () => true})).toThrowError(/does not name the page/);
        });
    });

    describe('page', () => {
        it('shows the Bridge panel for inspector.html?panel=typescript.bridge, from the monitor of its host in a frame', async () => {
            const frame = document.createElement('iframe');
            document.body.append(frame);
            try {
                const followed = fakeWebFront(), time = manualTime();
                followed.call('app.save');
                const host = {bridge: {ready: Promise.resolve()}, bridgeState: monitorOf(followed, time)};
                const view = {...panelView(), document: frame.contentDocument, parent: {webfrontInspectorHost: host},
                    location: {href: 'http://localhost/webfront/developer/environment/inspector.html?panel=typescript.bridge&frame=1'}};
                const shown = await main(view);
                expect(frame.contentDocument.title).toBe('WebFront inspector: TypeScript state: Bridge');
                expect(shown.fields.ready.textContent).toBe('resolved');
                expect([...frame.contentDocument.querySelectorAll('table.registry')[0].querySelectorAll('tbody tr')].map(row => row.children[0].textContent))
                    .toEqual(['app.save']);
                expect(frame.contentDocument.querySelector('dl.bridge')).not.toBeNull();
                expect(followed.probe).withContext('the panel reads: the bridge is followed').not.toBeNull();
                shown.poller.stop();
            } finally { frame.remove(); }
        });
    });
});
