// The registry of the inspectors in a real browser (#681, ADR-027 section 7): the functions that this suite's application
// registered are listed with their TypeScript signature, where they were registered and their counters, through the pages
// of webfront.inspector.functions, with the bridge's existing types (the exception of ADR-003: no new wire type).
//
// These specs run in the frame of wf test. Its connection is not the application's view: the inspectors refuse it, and
// the list is read as the inspector's own frame reads it, through the bridge of the page that holds the frame.

/** The bridge of the page that holds this frame: the environment's page, which is the proven view of the test driver. */
const hostBridge = () => window.parent.webfrontInspectorHost?.bridge ?? window.parent.webFront;

/** Every row of a paged inspector function, with what its first page gave beside the rows. */
async function allRows(bridge, name) {
    const rows = [];
    let cursor = 0, rest = [];
    for (let page = 0; page < 1000; ++page) {
        const [list, next, ...more] = await bridge.cppFunction(name)(cursor, 200);
        rows.push(...list);
        if (page === 0) rest = more;
        if (!next) return {rows, rest};
        expect(next).toBeGreaterThan(cursor);
        cursor = next;
    }
    throw new Error(`${name} never ended`);
}

const functionNamed = (rows, name) => rows.find(row => row[1] === name);

describe('The inspectors\' registry', () => {
    it('is refused to this frame, which is not a view of the application', async () => {
        for (const name of ['webfront.inspector.functions', 'webfront.inspector.jsFunctions', 'webfront.inspector.browsers']) {
            const error = await webFront.cppFunction(name)(0, 200).then(() => null, failure => failure);
            expect(error?.code).withContext(name).toBe('webfront.inspector.denied');
        }
    });

    it('describes the functions of this suite to the page that holds the frame', async () => {
        const host = hostBridge();
        await host.ready;
        const [, admitted] = await host.cppFunction('webfront.inspector.containers')();
        expect(admitted).withContext('the page of the test driver is a proven view').toBeTrue();

        const {rows} = await allRows(host, 'webfront.inspector.functions');
        // (identifier, name, signature, file:line, space, flags, calls, failures, last error, mean ms, longest ms)
        expect(rows.every(row => Array.isArray(row) && row.length === 11)).toBeTrue();
        expect(new Set(rows.map(row => row[0])).size).toBe(rows.length);
        expect(rows.some(row => row[1].startsWith('webfront.inspector.'))).withContext('the inspectors do not list themselves').toBeFalse();

        const echo = functionNamed(rows, 'echoString');
        expect(echo[2]).toBe('(a: string) => Promise<string>');
        expect(echo[3]).toMatch(/JasmineTest\.cpp:\d+$/);
        expect(echo[4]).toBe('');
        expect(functionNamed(rows, 'recordTupleFromJs')[2]).toBe('(a: [number, string]) => Promise<void>');
        expect(functionNamed(rows, 'listEcho')[2]).toBe('(a: string[]) => Promise<string[]>');
        expect(functionNamed(rows, 'bitsetEcho')[2]).toBe('(a: Uint8Array) => Promise<Uint8Array>');
        expect(functionNamed(rows, 'throwFromCpp')[2]).toBe('() => Promise<void>');

        // A function that receives the calling UI says so, and the UI is not one of its JavaScript parameters.
        const caller = functionNamed(rows, 'callBackCaller');
        expect(caller[5] & 1).toBe(1);
        expect(caller[2]).toBe('(a: string) => Promise<string>');
        expect(echo[5] & 1).toBe(0);
        // A function that answers later says so, and its result is that of its task.
        const deferred = functionNamed(rows, 'deferredEcho');
        expect(deferred[5] & 2).toBe(2);
        expect(deferred[2]).toBe('(a: string, b: number) => Promise<string>');

        // The environment's own callbacks are listed with the application's.
        expect(functionNamed(rows, 'webfront.developer.read')).toBeDefined();
    });

    it('counts the calls and the failures, and times the calls while it is read', async () => {
        const host = hostBridge();
        const read = async name => functionNamed((await allRows(host, 'webfront.inspector.functions')).rows, name);
        const before = await read('echoString');
        const failuresBefore = (await read('throwFromCpp'))[7];

        expect(await webFront.cppFunction('echoString')('inspected')).toBe('inspected');
        await expectAsync(webFront.cppFunction('throwFromCpp')()).toBeRejected();

        const after = await read('echoString');
        expect(after[6]).toBe(before[6] + 1);
        expect(after[7]).toBe(before[7]);
        // The read above opened the measure: this call was timed, and its durations are plausible.
        expect(after[5] & 4).toBe(4);
        expect(after[9]).toBeGreaterThanOrEqual(0);
        expect(after[10]).toBeGreaterThanOrEqual(after[9]);
        expect(after[10]).toBeLessThan(60000);
        const failed = await read('throwFromCpp');
        expect(failed[7]).toBe(failuresBefore + 1);
        expect(failed[8]).not.toBe('');
    });

    it('counts the JavaScript functions that C++ calls, and tells the connected browsers', async () => {
        const host = hostBridge();
        // (identifier, name, calls, failures of the typed calls). C++ offers its read binding to every page that connects:
        // that call was made at least for the page that holds this frame and for this frame.
        const called = (await allRows(host, 'webfront.inspector.jsFunctions')).rows;
        expect(called.every(row => Array.isArray(row) && row.length === 4)).toBeTrue();
        const binding = called.find(row => row[1] === 'wfBind.exchange');
        expect(binding?.[2]).toBeGreaterThanOrEqual(2);
        expect(binding?.[3]).toBe(0);

        const {rows, rest: [tasks]} = await allRows(host, 'webfront.inspector.browsers');
        // (identifier, flags, path, space, pending writes, awaited results, unanswered calls); the flags tell an embedded view
        // (1), a local client (2), a started browser (4), a closed one (8) and the page that asks (16).
        expect(rows.every(row => Array.isArray(row) && row.length === 7)).toBeTrue();
        const asking = rows.filter(row => row[1] & 16);
        expect(asking.length).toBe(1);
        expect(asking[0][1] & 7).withContext('the page that asks is a started, local, embedded view').toBe(7);
        // This frame has its own connection, which is neither the view nor a local client.
        expect(rows.some(row => (row[1] & 19) === 0 && (row[1] & 4) === 4)).toBeTrue();
        expect(Number.isSafeInteger(tasks) && tasks >= rows.length).withContext(`tasks: ${tasks}`).toBeTrue();
    });
});

// The trees of the watches in a real browser (#682, ADR-027 sections 3 to 6): nodes and pages travel with the bridge's
// existing types (lists of tuples, numbers, strings, a boolean), which the JavaScript codec decodes unchanged.
describe('The inspectors\' watches', () => {
    const inspector = name => hostBridge().cppFunction(`webfront.inspector.${name}`);
    const flags = {writable: 1, truncated: 2, removed: 4, depthReached: 8, childrenChanged: 16};
    let rows = 0, text = 0;

    /** Reads a pass to its end from its first page; returns every node it told. */
    async function finishPass(watch, page) {
        const nodes = [...page[0]];
        let [, next, revision] = page;
        while (next !== 0) {
            const [more, following, same, resync] = await inspector('changes')(watch, revision, next, 200);
            expect(resync).toBeFalse();
            expect(same).toBe(revision);
            nodes.push(...more);
            next = following;
        }
        return nodes;
    }

    beforeAll(async () => {
        await hostBridge().ready;
        [rows, text] = await webFront.cppFunction('watchFixture.declare')();
    });
    afterAll(() => webFront.cppFunction('watchFixture.remove')());

    it('are refused to this frame, which is not a view of the application', async () => {
        const calls = {limits: [], reset: [rows], children: [rows, '', 1, 0, 10], changes: [rows, 1, 0, 10], value: [rows, '', 0, 0],
            fold: [rows, '', 1], write: [rows, 1, '/#0', 'row 0', 'x']};
        for (const [name, args] of Object.entries(calls)) {
            const error = await webFront.cppFunction(`webfront.inspector.${name}`)(...args).then(() => null, failure => failure);
            expect(error?.code).withContext(name).toBe('webfront.inspector.denied');
        }
        expect(await webFront.cppFunction('watchFixture.row')(0)).toBe('row 0');
    });

    it('are discovered, then read page after page from the root', async () => {
        const [descriptors] = await inspector('watches')(0, 0, 200);
        const found = descriptors.find(descriptor => descriptor[0] === rows);
        expect(found.slice(1, 3)).toEqual(['suite.rows', 'sequence']);
        expect(found[3]).toMatch(/JasmineTest\.cpp:\d+$/);
        expect(found[4]).toBeTrue();
        const [maxWriteTextBytes, responseBudget, maxNodes, maxSnapshotBytes] = await inspector('limits')();
        expect(responseBudget).toBeGreaterThanOrEqual(4096);
        expect(maxWriteTextBytes).toBeGreaterThanOrEqual(1024);
        expect([maxNodes, maxSnapshotBytes]).toEqual([10000, 16 * 1024 * 1024]);

        const revision = await inspector('reset')(rows);
        let [nodes, next, answered, resync] = await inspector('children')(rows, '', revision, 0, 200);
        expect(resync).toBeFalse();
        // (path, kind, label, value, children, flags): the first node is the parent, here the root.
        expect(nodes[0]).toEqual(['', 'sequence', 'suite.rows', '', 500, 0]);
        expect(nodes[1]).toEqual(['/#0', 'string', '[0]', 'row 0', 0, flags.writable]);
        expect(nodes.length).toBe(201);
        expect(next).toBe(200);
        expect(answered).toBeGreaterThan(revision);
        let read = 200;
        while (next !== 0) {
            [nodes, next, answered] = await inspector('children')(rows, '', answered, next, 200);
            expect(nodes[0][0]).toBe('');
            read += nodes.length - 1;
        }
        expect(read).toBe(500);
        // A revision that is not the memory's: C++ emptied it, and says so.
        [nodes, next, answered, resync] = await inspector('children')(rows, '', revision, 0, 10);
        expect([nodes, next, resync]).toEqual([[], 0, true]);
        expect((await inspector('children')(rows, '', answered, 0, 10))[0].length).toBe(11);
    });

    it('tell their differences by passes, which an unfolding interrupts without losing any', async () => {
        let revision = await inspector('reset')(rows);
        let next = 0;
        do { [, next, revision] = await inspector('children')(rows, '', revision, next, 200); } while (next !== 0);
        await webFront.cppFunction('watchFixture.changeAll')('changed');
        const first = await inspector('changes')(rows, revision, 0, 50);
        expect(first[0].length).toBe(50);
        expect(first[0][0]).toEqual(['/#0', 'string', '[0]', 'changed 0', 0, flags.writable]);
        expect(first[1]).toBe(50);
        // An unfolding during the pass, with the revision of the pass: served at once, fresh, and no resynchronization.
        const [page, , answered, resync] = await inspector('children')(rows, '', first[2], 100, 10);
        expect(resync).toBeFalse();
        expect(page[1].slice(0, 4)).toEqual(['/#100', 'string', '[100]', 'changed 100']);
        // The next pass finds what the interrupted one had not served: 500, less the 50 served and the 10 just read.
        const rest = await finishPass(rows, await inspector('changes')(rows, answered, 0, 200));
        expect(rest.length).toBe(440);
        expect(rest.some(node => node[0] === '/#49' || node[0] === '/#105')).toBeFalse();
        expect(rest.every(node => node[3].startsWith('changed '))).toBeTrue();
        await webFront.cppFunction('watchFixture.changeAll')('row');
    });

    it('write a value only where it still shows what was displayed', async () => {
        const revision = await inspector('reset')(rows);
        const [, , opened] = await inspector('children')(rows, '', revision, 0, 10);
        const [written, code, message] = await inspector('write')(rows, opened, '/#3', 'row 3', 'écrit €');
        expect([code, message]).toEqual(['', '']);
        expect(written).toBeGreaterThan(opened);
        expect(await webFront.cppFunction('watchFixture.row')(3)).toBe('écrit €');
        // What the inspector showed is no longer true: the answer says so, with the revision to go on with.
        const [after, stale] = await inspector('write')(rows, written, '/#3', 'row 3', 'again');
        expect(stale).toBe('webfront.inspector.stale');
        expect(await webFront.cppFunction('watchFixture.row')(3)).toBe('écrit €');
        // A refusal seen before any effect rejects: a node that is not writable, an older revision.
        const refused = await inspector('write')(rows, after, '', '', 'x').then(() => null, failure => failure);
        expect(refused?.code).toBe('webfront.inspector.readOnly');
        const old = await inspector('write')(rows, opened, '/#4', 'row 4', 'x').then(() => null, failure => failure);
        expect(old?.code).toBe('webfront.inspector.stale');
        expect((await inspector('write')(rows, after, '/#3', 'écrit €', 'row 3'))[1]).toBe('');
    });

    it('give a full value by fragments of one snapshot, which never cut a character', async () => {
        const revision = await inspector('reset')(text);
        const [[root]] = await inspector('children')(text, '', revision, 0, 10);
        expect(root.slice(0, 3)).toEqual(['', 'string', 'suite.text']);
        expect(root[5]).toBe(flags.truncated);
        expect(new TextEncoder().encode(root[3]).length).toBeLessThanOrEqual(256);
        let [part, total, next, capped, snapshot] = await inspector('value')(text, '', 0, 0);
        let whole = part, fragments = 1;
        while (next !== 0) {
            [part, total, next, capped] = await inspector('value')(text, '', snapshot, next);
            expect(part.includes('�')).withContext('a fragment cut a character').toBeFalse();
            whole += part;
            ++fragments;
        }
        expect(fragments).toBeGreaterThan(2);
        expect(capped).toBeFalse();
        expect(total).toBe(new TextEncoder().encode(whole).length);
        expect(whole).toBe('héllo € '.repeat(whole.length / 8));
        expect(total).toBeGreaterThanOrEqual(100000);
        // The snapshot was freed with its last fragment: its identifier no longer reads anything.
        const late = await inspector('value')(text, '', snapshot, 4).then(() => null, failure => failure);
        expect(late?.code).toBe('webfront.inspector.stale');
    });

    it('answer that a watch is gone once its handle is destroyed', async () => {
        const revision = await inspector('reset')(rows);
        await webFront.cppFunction('watchFixture.remove')();
        const gone = await inspector('children')(rows, '', revision, 0, 10).then(() => null, failure => failure);
        expect(gone?.code).toBe('webfront.inspector.unknown');
        const [descriptors] = await inspector('watches')(0, 0, 200);
        expect(descriptors.some(descriptor => descriptor[0] === rows)).toBeFalse();
    });
});
