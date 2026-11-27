// The state of the bridge in the page, in a real browser (#684, ADR-027 section 8): a call to C++ that is pending is seen
// with its name, then is gone once C++ answered; a rejected Promise is seen with its reason; the JavaScript functions that
// C++ calls are told. The same state crosses the bridge as a list of tuples (the exception of ADR-003: no new wire
// type) when the page publishes it for a detached panel, which reads it back.
//
// These specs run in the frame of wf test, against the WebFront.js and the C++ of this checkout. They follow this frame's
// bridge through the probe that WebFront.js offers to an inspector, as the environment's monitor does (inspector-bridge.mjs,
// whose own specs are test/developer/specs/inspector-bridge-specs.mjs). The frame's connection is not the application's
// view: the relay is reached as the page that holds the frame reaches it, through that page's bridge.

const hostBridge = () => window.parent.webfrontInspectorHost?.bridge ?? window.parent.webFront;
const relayHost = 'jasmine-bridge-state';
const relay = {source: 'webfront.inspector.bridgeSource', publish: 'webfront.inspector.publishBridge', read: 'webfront.inspector.bridge'};

const reasonOf = error => error?.code && error.code !== 'webfront.error' ? `${error.code}: ${error.message}` : String(error?.message ?? error);

/** What follows a bridge through its probe: the pending calls, the rejections and the JavaScript calls, as rows of the relay. */
function follow(bridge) {
    const pending = new Set(), rejections = [], js = new Map();
    let sequence = 0;
    const track = (name, started, resolve, reject) => {
        const entry = {id: ++sequence, name, started};
        pending.add(entry);
        return [value => { pending.delete(entry); resolve(value); }, error => {
            pending.delete(entry);
            rejections.push({id: ++sequence, name, reason: reasonOf(error), at: Date.now()});
            reject(error);
        }];
    };
    const probe = {
        call: (name, resolve, reject) => track(name, performance.now(), resolve, reject),
        jsCall: name => js.set(name, (js.get(name) ?? 0) + 1),
    };
    // The calls made before the probe: WebFront.js keeps their name, not when they started.
    for (const call of bridge.pendingCalls.values()) [call.resolve, call.reject] = track(call.name, null, call.resolve, call.reject);
    bridge.probe = probe;
    const elapsed = started => started === null ? null : performance.now() - started;
    return {
        pending: () => [...pending].map(({name, started}) => ({name, elapsed: elapsed(started)})),
        rejections: () => [...rejections],
        jsCalls: name => js.get(name) ?? 0,
        rows: () => [
            ['ready', !bridge.readySettled ? 'pending' : bridge.readyError ? 'rejected' : 'resolved', String(bridge.readyError?.message ?? ''), 0, 0],
            ['connection', bridge.state, '', 0, 0],
            ...[...pending].map(call => ['pending', call.name, '', call.id, elapsed(call.started) ?? -1]),
            ...rejections.map(rejection => ['rejected', rejection.name, rejection.reason, rejection.id, rejection.at]),
            ...[...js].map(([name, calls], index) => ['js', name, '', index + 1, calls]),
        ],
        release() { if (bridge.probe === probe) bridge.probe = null; },
    };
}

/** Every row that the relay gives for `host`, with the publication, the age and the flags of the last page. */
async function readRelay(bridge, host) {
    const rows = [];
    for (let cursor = 0, pages = 0; pages < 100; ++pages) {
        const [list, next, publication, age, flags] = await bridge.cppFunction(relay.read)(host, cursor, 200);
        rows.push(...list);
        if (!next) return {rows, publication, age, flags};
        expect(next).toBeGreaterThan(cursor);
        cursor = next;
    }
    throw new Error('The relay never ended');
}

describe('The state of the bridge in the page', () => {
    let followed;

    beforeAll(() => webFront.ready);
    afterEach(() => {
        followed?.release();
        followed = null;
    });

    it('records nothing in a bridge that no inspector reads', async () => {
        expect(webFront.probe).toBeNull();
        expect(webFront.readyError).toBeNull();
        const answer = webFront.cppFunction('deferredEcho')('unobserved', 100);
        // Only the name of a pending call is kept, for an inspector that would come later.
        const calls = [...webFront.pendingCalls.values()];
        expect(calls.map(call => call.name)).toContain('deferredEcho');
        expect(calls.every(call => Object.keys(call).sort().join() === 'name,reject,resolve')).toBeTrue();
        expect(await answer).toBe('deferred:unobserved');
    });

    it('shows a call that is pending with its duration, then no longer once C++ answered', async () => {
        followed = follow(webFront);
        const answer = webFront.cppFunction('deferredEcho')('still pending', 400);
        await new Promise(resolve => setTimeout(resolve, 50));
        const pending = followed.pending().find(call => call.name === 'deferredEcho');
        expect(pending).withContext(JSON.stringify(followed.pending())).toBeDefined();
        expect(pending.elapsed).toBeGreaterThanOrEqual(40);

        expect(await answer).toBe('deferred:still pending');
        expect(followed.pending().some(call => call.name === 'deferredEcho')).toBeFalse();
        expect(followed.rejections()).toEqual([]);
    });

    it('lets an inspector adopt a call that was made before it came, by its name', async () => {
        const answer = webFront.cppFunction('deferredEcho')('made before', 300);
        followed = follow(webFront);
        expect(followed.pending().find(call => call.name === 'deferredEcho')?.elapsed).toBeNull();
        expect(await answer).toBe('deferred:made before');
        expect(followed.pending().some(call => call.name === 'deferredEcho')).toBeFalse();
    });

    it('shows a rejected Promise with its function, its reason and its time', async () => {
        followed = follow(webFront);
        const started = Date.now();
        await expectAsync(webFront.cppFunction('throwFromCpp')()).toBeRejected();
        await expectAsync(webFront.cppFunction('throwCodedFromCpp')()).toBeRejected();
        await expectAsync(webFront.cppFunction('noSuchFunctionOfTheSuite')()).toBeRejected();
        // A call that cannot even be encoded is rejected before it is sent: it is seen too.
        await expectAsync(webFront.cppFunction('echoString')(Symbol('not a wire value'))).toBeRejected();
        expect(followed.rejections().length).toBe(4);
        const [plain, coded, missing, unsent] = followed.rejections();
        expect(plain.name).toBe('throwFromCpp');
        expect(plain.reason).toContain('C++ callback failed');
        expect(plain.at).toBeGreaterThanOrEqual(started);
        expect(coded).toEqual(jasmine.objectContaining({name: 'throwCodedFromCpp', reason: 'denied: the user said no'}));
        expect(missing.reason).toMatch(/^webfront\.notFound: /);
        expect(unsent.name).toBe('echoString');
        expect(followed.pending()).toEqual([]);
    });

    it('tells the JavaScript functions that C++ calls', async () => {
        const tokens = [];
        globalThis.bridgeStateSpecs = {called: token => tokens.push(token)};
        try {
            followed = follow(webFront);
            expect(await webFront.cppFunction('callBridgeStateSpecs')('one')).toBe('called:one');
            expect(await webFront.cppFunction('callBridgeStateSpecs')('two')).toBe('called:two');
            // C++ sends its call to JavaScript before the answer of the call that made it: both were received by now.
            expect(tokens).toEqual(['one', 'two']);
            expect(followed.jsCalls('bridgeStateSpecs.called')).toBe(2);
        } finally { delete globalThis.bridgeStateSpecs; }
    });

    describe('relayed by C++ for a detached panel', () => {
        it('is refused to this frame, which is not a view of the application', async () => {
            const refused = async (name, ...args) => (await webFront.cppFunction(name)(...args).then(() => null, failure => failure))?.code;
            followed = follow(webFront);
            const rows = followed.rows();
            followed.release();  // the refusals below are not what these rows tell
            expect(await refused(relay.source, relayHost)).toBe('webfront.inspector.denied');
            expect(await refused(relay.publish, relayHost, webFront.list(rows))).toBe('webfront.inspector.denied');
            expect(await refused(relay.read, relayHost, 0, 200)).toBe('webfront.inspector.denied');
        });

        it('asks the page to publish when a panel reads, and gives the panel the state that the page published', async () => {
            const host = hostBridge();
            await host.ready;
            // What C++ calls in the page that announced itself: the environment's own publisher, if any, is set aside meanwhile.
            const page = window.parent, publisher = page.wfInspect;
            let asked = 0;
            page.wfInspect = {publish: () => { ++asked; }};
            try {
                expect(await host.cppFunction(relay.source)(relayHost)).withContext('nobody reads yet').toBeFalse();
                // (rows, next, publication, age, flags): the page is known (1) and has published nothing (2 is not set).
                expect(await readRelay(host, relayHost)).toEqual({rows: [], publication: 0, age: 0, flags: 1});
                for (let waited = 0; !asked && waited < 100; ++waited) await new Promise(resolve => setTimeout(resolve, 20));
                expect(asked).withContext('C++ asked the page to publish').toBe(1);

                followed = follow(webFront);
                const pendingCall = webFront.cppFunction('deferredEcho')('relayed', 400);
                await expectAsync(webFront.cppFunction('throwCodedFromCpp')()).toBeRejected();
                const published = followed.rows();
                expect(published.map(row => row[0])).toEqual(['ready', 'connection', 'pending', 'rejected']);
                expect(await host.cppFunction(relay.publish)(relayHost, host.list(published))).withContext('a panel reads: go on publishing').toBeTrue();

                const read = await readRelay(host, relayHost);
                // The rows cross the bridge both ways as a list of tuples of the existing types, unchanged.
                expect(read.rows).toEqual(published);
                expect(read.publication).toBeGreaterThan(0);
                expect(read.age).toBeGreaterThanOrEqual(0);
                expect(read.flags).toBe(3);
                expect(read.rows[0].slice(0, 2)).toEqual(['ready', 'resolved']);
                expect(read.rows[1].slice(0, 2)).toEqual(['connection', 'linked']);
                expect(read.rows[2].slice(0, 2)).toEqual(['pending', 'deferredEcho']);
                expect(read.rows[3].slice(0, 3)).toEqual(['rejected', 'throwCodedFromCpp', 'denied: the user said no']);
                expect(await pendingCall).toBe('deferred:relayed');
                // A state of more rows than the relay keeps is refused, and the earlier one stays.
                const tooMany = Array.from({length: 257}, (unused, index) => ['js', `f${index}`, '', index, 1]);
                await expectAsync(host.cppFunction(relay.publish)(relayHost, host.list(tooMany))).toBeRejected();
                expect((await readRelay(host, relayHost)).rows).toEqual(published);
            } finally {
                if (publisher === undefined) delete page.wfInspect; else page.wfInspect = publisher;
            }
        });
    });
});
