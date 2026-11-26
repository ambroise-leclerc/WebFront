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
