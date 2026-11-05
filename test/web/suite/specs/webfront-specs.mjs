import './react-specs.mjs';
import './fallback-specs.mjs';
import './dom-document-specs.mjs';
import './forms-specs.mjs';
import './content-policy-specs.mjs';
import './dom-specs.mjs';
import { importedValue } from './module-fixture.mjs';

// The page of the application is covered by the lifecycle cases (path-encoding): these specs run in the frame of wf test.
describe('Frontend page URL', () => {
    it('serves a filename containing a literal plus through the C++ URL builder', async () => {
        const url = await webFront.cppFunction('plusPageUrl')();
        expect(new URL(url).pathname).toBe('/a%2Bb.html');
        const response = await fetch(url);
        expect(response.ok).toBeTrue();
        expect(await response.text()).toContain('<title>Plus in a path</title>');
    });
    it('injects a C++ script when the handshake completes before the body exists', () => {
        const body = document.body;
        const text = new TextEncoder().encode('globalThis.earlyInjectedScript = true;');
        const packet = new DataView(new ArrayBuffer(4 + text.length));
        packet.setUint8(0, 2); // TextCommand
        packet.setUint8(1, 1); // injectScript
        packet.setUint16(2, text.length);
        new Uint8Array(packet.buffer, 4).set(text);
        try {
            body.remove();
            expect(document.body).toBeNull();
            expect(() => webFront.handleTextCommand(packet)).not.toThrow();
            expect(globalThis.earlyInjectedScript).toBeTrue();
        } finally {
            document.documentElement.append(body);
            delete globalThis.earlyInjectedScript;
        }
    });
    it('runs in the frame of the test runner, served from the origin of the application', () => {
        expect(location.hostname).toBe('127.0.0.1');
        expect(window.parent).not.toBe(window);
    });
});

const cppToken = 'cpp-to-js-token';
const jsToken = 'js-to-cpp-token';
const status = document.body.appendChild(document.createElement('p'));
status.textContent = 'Waiting for the C++/JavaScript bridge…';

const browserReadyTimeoutMs = 10000;

async function notifyBrowserReady() {
    const deadline = Date.now() + browserReadyTimeoutMs;
    for (;;) {
        try {
            await webFront.cppFunction('browserReady')();
            return;
        } catch (error) {
            if (!(error instanceof Error) || error.message !== 'WebFront bridge is not connected')
                throw error;
            if (Date.now() >= deadline)
                throw new Error(`notifyBrowserReady() timed out after ${browserReadyTimeoutMs}ms waiting for the WebFront bridge to connect`);
            await new Promise(resolve => setTimeout(resolve, 10));
        }
    }
}

let resolveCppCall;
const cppCall = new Promise((resolve) => {
    resolveCppCall = resolve;
});

let resolveCppArraysCall;
const cppArraysCall = new Promise((resolve) => {
    resolveCppArraysCall = resolve;
});

let resolveCallerCall;
const callerCall = new Promise((resolve) => {
    resolveCallerCall = resolve;
});

let resolveBroadcast;
const broadcastCall = new Promise((resolve) => {
    resolveBroadcast = resolve;
});

let resolveCppResult;
const cppResultCall = new Promise((resolve) => {
    resolveCppResult = resolve;
});

let finishSenderChecks;
const senderChecks = new Promise(resolve => { finishSenderChecks = resolve; });
let senderStarted;
const senderSlowStarted = new Promise(resolve => { senderStarted = resolve; });
let senderStopped;
const senderCancelled = new Promise(resolve => { senderStopped = resolve; });
let finishSlowSender;

let finishListChecks;
const listChecks = new Promise(resolve => { finishListChecks = resolve; });

let finishErrorChecks;
const errorChecks = new Promise(resolve => { finishErrorChecks = resolve; });

let finishBitsetChecks;
const bitsetChecks = new Promise(resolve => { finishBitsetChecks = resolve; });

let finishBitsetFutureChecks;
const bitsetFutureChecks = new Promise(resolve => { finishBitsetFutureChecks = resolve; });

function isPackedBitset(value, expected) {
    if (!(value instanceof Uint8Array)) return false;
    if (value.length !== expected.length) return false;
    return expected.every((byte, index) => value[index] === byte);
}

globalThis.webfrontTests = {
    bitsetEcho(value, delayed = false) {
        if (!isPackedBitset(value, [0x81, 1]))
            throw new Error('Incorrect C++ packed bitset');
        return delayed ? new Promise(resolve => setTimeout(() => resolve(value), 50)) : value;
    },
    bitsetBad(kind, delayed = false) {
        const value = [new Uint8Array([0, 2]), new Uint8Array(1), new Int8Array(2)][kind];
        // Deliberately reply out of dispatch order while the future worker is waiting.
        return delayed ? new Promise(resolve => setTimeout(() => resolve(value), (3 - kind) * 20)) : value;
    },
    bitsetChecksFinished(passed) { finishBitsetChecks(passed); },
    bitsetFutureChecksFinished(passed) { finishBitsetFutureChecks(passed); },
    listEcho(value) {
        if (!Array.isArray(value)) throw new Error('A C++ list must arrive as an array');
        return globalThis.webFront.list(value);
    },
    recordsEcho(value) {
        if (!Array.isArray(value) || !value.every(Array.isArray)) throw new Error('A C++ list of tuples must arrive as an array of arrays');
        if (!(value[0][3] instanceof Uint8Array)) throw new Error('A record field must keep its typed array');
        return globalThis.webFront.list(value);
    },
    heterogeneousList() { return globalThis.webFront.list(['text', 1]); },
    rejectWithCode(code) {
        return Promise.reject(Object.assign(new Error(`failure ${code}`), {code}));
    },
    rejectLong() { return Promise.reject(Object.assign(new Error('\u00e9'.repeat(40000)), {code: 'long'})); },
    errorChecksFinished(passed) { finishErrorChecks(passed); },
    listChecksFinished(passed) { finishListChecks(passed); },
    senderValue(value) { return Promise.resolve(`sender:${value}`); },
    senderVoid() {},
    senderThrows() { return Promise.reject(new Error('sender browser failure')); },
    senderChecksFinished(passed) { finishSenderChecks(passed); },
    senderSlow() {
        senderStarted();
        return new Promise(resolve => { finishSlowSender = resolve; });
    },
    senderWasCancelled() { senderStopped(); },
    receiveFromCpp(token) {
        status.textContent = `C++ called served JavaScript with: ${token}`;
        resolveCppCall(token);
    },

    receiveArraysFromCpp(...arrays) {
        resolveCppArraysCall(arrays);
    },

    receiveFromCaller(token) {
        resolveCallerCall(token);
    },

    receiveBroadcast(token, number) {
        resolveBroadcast([token, number]);
    },

    returnToCpp(value) {
        const result = `js-result:${value}`;
        resolveCppResult(result);
        return result;
    }
};

describe('WebFront browser integration', () => {
    it('supports packed bitsets in C++ future and sender calls and rejects malformed returns', async () => {
        await expectAsync(bitsetChecks).toBeResolvedTo(true);
        await expectAsync(bitsetFutureChecks).toBeResolvedTo(true);
    });

    it('exchanges bitset callback arguments/results as Uint8Array including tuples and empty grids', async () => {
        const echo = webFront.cppFunction('bitsetEcho');
        const result = await echo(new Uint8Array([0x81, 1]));
        expect(result instanceof Uint8Array).toBeTrue();
        expect(Array.from(result)).toEqual([0x81, 1]);
        expect(Array.from(await webFront.cppFunction('bitsetEmpty')(new Uint8Array()))).toEqual([]);
        const grid = new Uint8Array(128); grid[0] = 1; grid[127] = 128;
        expect(Array.from(await webFront.cppFunction('bitsetGrid')(grid))).toEqual(Array.from(grid));
        const tuple = await webFront.cppFunction('bitsetTuple')([result, 42]);
        expect(Array.from(tuple[0])).toEqual([0x81, 1]); expect(tuple[1]).toBe(42);
        for (const value of [new Uint8Array([0, 2]), new Uint8Array(1), new Uint8Array(3), new Int8Array(2), '01'])
            await expectAsync(echo(value)).toBeRejected();
    });

    it('exchanges lists with C++ futures and senders, and refuses a heterogeneous one', async () => {
        await expectAsync(listChecks).toBeResolvedTo(true);
    });

    it('receives lists of strings and of records from C++ callbacks', async () => {
        expect(await webFront.cppFunction('listFromCpp')(0)).toEqual([]);
        const many = await webFront.cppFunction('listFromCpp')(3000);
        expect(many.length).toBe(3000);
        expect(many[0]).toBe('entry-0');
        expect(many[2999]).toBe('entry-2999');
        expect(await webFront.cppFunction('pageFromCpp')()).toEqual([['a', 'b'], 'cursor']);
        // The headers of the elements share the policy's 32 KiB buffer: a longer list is rejected explicitly, not truncated.
        const tooMany = await webFront.cppFunction('listFromCpp')(100000).then(() => null, error => error);
        expect(tooMany.message).toMatch(/encode buffer overflow/);
    });

    it('sends lists to C++ as marked lists, and short plain arrays as tuples C++ also accepts', async () => {
        const echo = webFront.cppFunction('listEcho');
        expect(await echo(webFront.list(['one', '', 'x'.repeat(70000)]))).toEqual(['one', '', 'x'.repeat(70000)]);
        expect(await echo(webFront.list([]))).toEqual([]);
        expect(await echo(['plain', 'array'])).toEqual(['plain', 'array']);
        const big = Array.from({length: 300}, (_, index) => `item-${index}`);
        expect(await echo(webFront.list(big))).toEqual(big);
        const records = [['a.txt', 12.5, true, new Uint8Array([1, 2, 3])], ['b', 0, false, new Uint8Array(300)]];
        const echoed = await webFront.cppFunction('recordsEcho')(webFront.list(records));
        expect(echoed.map(record => record.slice(0, 3))).toEqual([['a.txt', 12.5, true], ['b', 0, false]]);
        expect(Array.from(echoed[0][3])).toEqual([1, 2, 3]);
        expect(echoed[1][3].length).toBe(300);
    });

    it('accepts tuples nested 32 deep in a list element, in both codecs, and refuses 33', async () => {
        const nest = levels => { let value = 'x'; for (let level = 0; level < levels; ++level) value = [value]; return value; };
        const depthOf = value => { let depth = 0; while (Array.isArray(value)) { value = value[0]; ++depth; } return depth; };
        const echoed = await webFront.cppFunction('deepEcho')(webFront.list([nest(32)]));
        expect(echoed.length).toBe(1);
        expect(depthOf(echoed[0])).toBe(32);
        await expectAsync(webFront.cppFunction('deepEcho')(webFront.list([nest(33)])))
            .toBeRejectedWithError(TypeError, /at most 32 deep/);
        await expectAsync(webFront.cppFunction('listEcho')(webFront.list(['still works']))).toBeResolvedTo(['still works']);
    });

    it('refuses absurdly deep tuples in a list element at the limit, on sending and on receiving, without exhausting the stack', async () => {
        // Sending: 200,000 nested arrays are refused at depth 33, long before the recursion could overflow.
        const nest = levels => { let value = 'x'; for (let level = 0; level < levels; ++level) value = [value]; return value; };
        await expectAsync(webFront.cppFunction('deepEcho')(webFront.list([nest(200000)])))
            .toBeRejectedWithError(TypeError, /at most 32 deep/);
        // Receiving: a list whose one element is 200,000 one-field tuples (2 bytes of header each) around a boolean.
        const levels = 200000;
        const bytes = new Uint8Array(5 + 2 * levels + 1);
        const view = new DataView(bytes.buffer);
        view.setUint8(0, 20);                                // list
        view.setUint32(1, 1, webFront.littleEndian);         // one element
        for (let level = 0; level < levels; ++level) { bytes[5 + 2 * level] = 18; bytes[6 + 2 * level] = 1; }  // tuple of one field
        bytes[5 + 2 * levels] = 1;                           // booleanTrue
        expect(() => webFront.decodeParameters(1, view)).toThrowError(TypeError, /at most 32 deep/);
        expect(webFront.listElementDepth).toBeUndefined();   // the counter is restored whatever happened
        await expectAsync(webFront.cppFunction('listEcho')(webFront.list(['still works']))).toBeResolvedTo(['still works']);
    });

    it('refuses to send a heterogeneous or nested list, and C++ refuses a wrong element type', async () => {
        const echo = webFront.cppFunction('listEcho');
        await expectAsync(echo(webFront.list(['text', new Uint8Array(1)]))).toBeRejectedWithError(TypeError, /not of one type/);
        await expectAsync(echo(webFront.list([webFront.list(['a'])]))).toBeRejectedWithError(TypeError, /cannot contain lists/);
        await expectAsync(echo(webFront.list([[webFront.list(['a'])]]))).toBeRejectedWithError(TypeError, /cannot contain lists/);
        await expectAsync(echo(webFront.list([1, 2]))).toBeRejectedWithError(TypeError, /typed array/);
        await expectAsync(echo(webFront.list([true, false]))).toBeRejectedWithError(TypeError, /typed array/);
        await expectAsync(echo(webFront.list([[1, 2], [3, 4]]))).toBeRejectedWithError(/wrong parameter type|Wrong parameter type/i);
        await expectAsync(echo(webFront.list(['still', 'works']))).toBeResolvedTo(['still', 'works']);
    });

    it('resumes C++ coroutines with sender values, void completion and exceptions', async () => {
        await expectAsync(senderChecks).toBeResolvedTo(true);
    });

    it('cancels a pending C++ sender and ignores its late JavaScript result', async () => {
        await senderSlowStarted;
        await webFront.cppFunction('cancelBrowserSender')();
        await senderCancelled;
        finishSlowSender('late result');
        await Promise.resolve();
        expect(await webFront.cppFunction('senderCancellationObserved')()).toBeTrue();
    });
    it('loads native ES modules with relative imports', () => {
        expect(importedValue).toBe('relative-module-import-loaded');
    });

    it('allows C++ to call a function defined by a served module', async () => {
        await expectAsync(cppCall).toBeResolvedTo(cppToken);
    });

    it('allows served JavaScript to call a registered C++ function', () => {
        const recordFromJs = webFront.cppFunction('recordFromJs');
        expect(() => recordFromJs(jsToken)).not.toThrow();
    });

    it('delivers a browser frame with a 16-bit extended payload length to C++', () => {
        expect(() => webFront.cppFunction('recordExtendedFrameFromJs')('x'.repeat(126))).not.toThrow();
    });

    it('decodes every C++ numeric array type as the matching JavaScript typed array', async () => {
        const arrays = await cppArraysCall;
        const constructors = [
            Uint8Array, Int8Array, Uint16Array, Int16Array, Uint32Array,
            Int32Array, BigUint64Array, BigInt64Array, Float32Array, Float64Array
        ];
        expect(arrays.map(value => value.constructor)).toEqual(constructors);
        expect(Array.from(arrays[0])).toEqual([0, 255]);
        expect(Array.from(arrays[1])).toEqual([-128, 127]);
        expect(Array.from(arrays[2])).toEqual([0, 65535]);
        expect(Array.from(arrays[3])).toEqual([-32768, 32767]);
        expect(Array.from(arrays[4])).toEqual([0, 4294967295]);
        expect(Array.from(arrays[5])).toEqual([-2147483648, 2147483647]);
        expect(Array.from(arrays[6])).toEqual([0n, 18446744073709551615n]);
        expect(Array.from(arrays[7])).toEqual([-9223372036854775808n, 9223372036854775807n]);
        expect(Array.from(arrays[8])).toEqual([-1.5, 42.25]);
        expect(Array.from(arrays[9])).toEqual([-1.5, 42.25]);
    });

    it('encodes every JavaScript typed array type for an owning C++ callback', () => {
        const recordArrays = webFront.cppFunction('recordArraysFromJs');
        expect(() => recordArrays(
            new Uint8Array([0, 255]),
            new Int8Array([-128, 127]),
            new Uint16Array([0, 65535]),
            new Int16Array([-32768, 32767]),
            new Uint32Array([0, 4294967295]),
            new Int32Array([-2147483648, 2147483647]),
            new BigUint64Array([0n, 18446744073709551615n]),
            new BigInt64Array([-9223372036854775808n, 9223372036854775807n]),
            new Float32Array([-1.5, 42.25]),
            new Float64Array([-1.5, 42.25])
        )).not.toThrow();
    });

    it('keeps ordinary JavaScript arrays as heterogeneous tuples', () => {
        const recordTuple = webFront.cppFunction('recordTupleFromJs');
        expect(() => recordTuple([42, 'tuple'])).not.toThrow();
    });

    it('returns successful C++ calls as promises', async () => {
        await expectAsync(webFront.cppFunction('returnFromCpp')('from-js'))
            .toBeResolvedTo('cpp-result:from-js');
    });

    it('cuts a rejection message to what the maximum message size leaves', async () => {
        const bridge = webFront;
        const limit = bridge.maxMessageBytes;
        const sent = [];
        const send = bridge.socket.send.bind(bridge.socket);
        bridge.socket.send = buffer => { sent.push(buffer.byteLength); };  // captured, not sent: no C++ call is waiting
        try {
            bridge.maxMessageBytes = 100;
            bridge.sendFunctionError(0, Object.assign(new Error('x'.repeat(1000)), {code: 'tight'}));
        } finally { bridge.maxMessageBytes = limit; bridge.socket.send = send; }
        expect(sent).toEqual([100]);
    });

    it('still answers a C++ call whose rejection has a throwing code or message accessor', () => {
        const sent = [];
        const send = webFront.socket.send.bind(webFront.socket);
        webFront.socket.send = buffer => { sent.push(new Uint8Array(buffer.slice(0))); };
        const text = bytes => new TextDecoder().decode(bytes.subarray(10));
        try {
            const hostileCode = Object.defineProperty(new Error('visible'), 'code', {get() { throw new Error('getter'); }});
            const hostileMessage = Object.defineProperty(new Error('x'), 'message', {get() { throw new Error('getter'); }});
            const hostileString = {toString() { throw new Error('toString'); }};
            for (const error of [hostileCode, hostileMessage, hostileString])
                expect(() => webFront.sendFunctionError(0, error)).not.toThrow();
        } finally { webFront.socket.send = send; }
        expect(sent.length).toBe(3);
        for (const frame of sent) expect(text(frame)).toContain('webfront.error');
        expect(text(sent[0])).toContain('visible');
    });

    it('rejects with the code of the failure: application, generic and reserved', async () => {
        const denied = await webFront.cppFunction('throwCodedFromCpp')().then(() => null, error => error);
        expect(denied instanceof Error).toBeTrue();
        expect(denied.code).toBe('denied');
        expect(denied.message).toBe('the user said no');
        const plain = await webFront.cppFunction('throwFromCpp')().then(() => null, error => error);
        expect(plain.code).toBe('webfront.error');
        const missing = await webFront.cppFunction('missingCppFunction')().then(() => null, error => error);
        expect(missing.code).toBe('webfront.notFound');
        const malformed = await webFront.cppFunction('integerInCpp')(NaN).then(() => null, error => error);
        expect(malformed.code).toBe('webfront.malformed');
        expect(() => { 'use strict'; missing.code = 'webfront.cancelled'; }).toThrow();
        const mistyped = await webFront.cppFunction('integerInCpp')('text').then(() => null, error => error);
        expect(mistyped.code).toBe('webfront.malformed');
    });

    it('reports to C++ the code of a rejection, and refuses a reserved code from the application', async () => {
        await expectAsync(errorChecks).toBeResolvedTo(true);
    });

    it('rejects a call on a bridge that is not connected with the disconnected code', async () => {
        const url = await webFront.cppFunction('rejectedWebSocketUrl')();
        const failingBridge = new WebFrontBridge(url);
        await failingBridge.ready.catch(() => {});
        const error = await failingBridge.cppFunction('returnFromCpp')('x').then(() => null, failure => failure);
        expect(error.code).toBe('webfront.disconnected');
    });

    it('resolves a Promise when a C++ coroutine finishes later, and rejects it with its code', async () => {
        const started = performance.now();
        await expectAsync(webFront.cppFunction('deferredEcho')('x', 150)).toBeResolvedTo('deferred:x');
        expect(performance.now() - started).toBeGreaterThanOrEqual(100);
        const error = await webFront.cppFunction('deferredFailure')().then(() => null, failure => failure);
        expect(error.code).toBe('late:failure');
        expect(error.message).toBe('rejected after a delay');
    });

    it('answers concurrent deferred calls in the order they finish', async () => {
        const echo = webFront.cppFunction('deferredEcho');
        const order = [];
        const calls = [300, 50, 150].map(delay => echo(String(delay), delay).then(value => order.push(value)));
        await Promise.all(calls);
        expect(order).toEqual(['deferred:50', 'deferred:150', 'deferred:300']);
    });

    it('rejects missing functions and C++ exceptions', async () => {
        await expectAsync(webFront.cppFunction('missingCppFunction')())
            .toBeRejectedWithError(/was not found/);
        await expectAsync(webFront.cppFunction('throwFromCpp')())
            .toBeRejectedWithError('C++ callback failed');
    });

    it('receives C++ strings on both sides of the 16-bit length boundary', async () => {
        const longStringFromCpp = webFront.cppFunction('longStringFromCpp');
        for (const size of [255, 256, 65535, 65536, 65537, 1 << 20]) {
            const text = await longStringFromCpp(size);
            expect(text.length).toBe(size);
            expect(text === 'x'.repeat(size)).toBeTrue();
        }
    });

    it('echoes strings of 65536 bytes or more, multi-byte characters included, in both directions', async () => {
        const echoString = webFront.cppFunction('echoString');
        const euros = '\u20ac'.repeat(40000);  // 120000 UTF-8 bytes
        expect(await echoString(euros) === euros).toBeTrue();
        const straddling = 'x'.repeat(65534) + '\u00e9' + 'y'.repeat(10);  // the 2-byte character crosses the 65535 boundary
        expect(await echoString(straddling) === straddling).toBeTrue();
    });

    it('refuses a message past the maximum before sending it, and keeps the bridge usable', async () => {
        const echoString = webFront.cppFunction('echoString');
        const limit = webFront.maxMessageBytes;
        await expectAsync(echoString('x'.repeat(limit)))
            .toBeRejectedWithError(RangeError, /exceeds the maximum of/);
        await expectAsync(echoString('still alive')).toBeResolvedTo('still alive');
    });

    it('round-trips empty strings and rejects numbers outside C++ integer range', async () => {
        await expectAsync(webFront.cppFunction('returnFromCpp')('')).toBeResolvedTo('cpp-result:');
        const integerInCpp = webFront.cppFunction('integerInCpp');
        for (const value of [NaN, Infinity, -Infinity, 2147483648, -2147483649]) {
            await expectAsync(integerInCpp(value)).toBeRejectedWithError(/integer result range/);
        }
        await expectAsync(integerInCpp(2147483647)).toBeResolvedTo(2147483647);
        await expectAsync(integerInCpp(-2147483648)).toBeResolvedTo(-2147483648);
    });

    it('sends strings on both sides of the 16-bit length boundary', async () => {
        const lengthInCpp = webFront.cppFunction('lengthInCpp');
        await expectAsync(lengthInCpp('x'.repeat(65535))).toBeResolvedTo(65535);
        await expectAsync(lengthInCpp('x'.repeat(65536))).toBeResolvedTo(65536);
        await expectAsync(lengthInCpp('x'.repeat(3 << 20))).toBeResolvedTo(3 << 20);
    });

    it('receives a callback message and then its return value on the same connection', async () => {
        await expectAsync(webFront.cppFunction('callBackCaller')('caller-token'))
            .toBeResolvedTo('cpp-result:caller-token');
        await expectAsync(callerCall).toBeResolvedTo('caller-token');
    });

    it('receives a call that C++ broadcast to every browser from another thread', async () => {
        await expectAsync(webFront.cppFunction('broadcastFromThread')('broadcast-token')).toBeResolved();
        await expectAsync(broadcastCall).toBeResolvedTo(['broadcast-token', 401]);
    });

    it('allows C++ to await a JavaScript result', async () => {
        await expectAsync(cppResultCall).toBeResolvedTo('js-result:from-cpp');
    });

    it('resolves webFront.ready once the bridge has linked', async () => {
        await expectAsync(webFront.ready).toBeResolved();
    });

    it('rejects a bridge whose initial connection fails', async () => {
        // C++ owns this HTTP endpoint on an OS-assigned port and refuses every upgrade.
        const url = await webFront.cppFunction('rejectedWebSocketUrl')();
        const failingBridge = new WebFrontBridge(url);
        await expectAsync(failingBridge.ready).toBeRejectedWithError('WebFront connection failed');
    });

    it('does not report an unhandled rejection when ready is never consumed', async () => {
        const url = await webFront.cppFunction('rejectedWebSocketUrl')();
        const unhandled = [];
        const onUnhandled = event => unhandled.push(event.reason);
        globalThis.addEventListener('unhandledrejection', onUnhandled);
        try {
            const failingBridge = new WebFrontBridge(url);
            await new Promise(resolve => failingBridge.socket.addEventListener('close', resolve, {once: true}));
            // Let the browser dispatch any unhandledrejection queued by the socket failure.
            await new Promise(resolve => setTimeout(resolve, 0));
            expect(unhandled).toEqual([]);
        } finally {
            globalThis.removeEventListener('unhandledrejection', onUnhandled);
        }
    });
});

// A dynamic route of the HTTP server (#454): the reply is produced by C++ at run time and streamed in chunks.
describe('Dynamic HTTP routes', () => {
    const url = '/webfront/services/test-stream/pattern.bin';
    const size = 100000; // several chunks of the server's emission buffer
    const byteAt = index => (index * 7 + 3) % 251;

    it('streams a reply produced by C++', async () => {
        const response = await fetch(`${url}?size=${size}`);
        expect(response.status).toBe(200);
        expect(response.headers.get('content-type')).toBe('application/octet-stream');
        expect(response.headers.get('cache-control')).toBe('no-store');
        expect(response.headers.get('content-length')).toBe(String(size));
        const bytes = new Uint8Array(await response.arrayBuffer());
        expect(bytes.length).toBe(size);
        let wrong = -1;
        for (let index = 0; index < size && wrong < 0; ++index) if (bytes[index] !== byteAt(index)) wrong = index;
        expect(wrong).toBe(-1);
    });

    it('answers HEAD with the headers only', async () => {
        const response = await fetch(`${url}?size=${size}`, {method: 'HEAD'});
        expect(response.status).toBe(200);
        expect(response.headers.get('content-length')).toBe(String(size));
        expect((await response.arrayBuffer()).byteLength).toBe(0);
    });

    it('refuses the other methods and answers 500 when the handler fails', async () => {
        expect((await fetch(url, {method: 'POST'})).status).toBe(405);
        expect((await fetch(`${url}?fail=1`)).status).toBe(500);
    });

    it('leaves the embedded WebFront.js to the core', async () => {
        const response = await fetch('/WebFront.js');
        expect(response.ok).toBeTrue();
        expect(response.headers.get('content-type')).toContain('javascript');
    });
});

// Byte ranges of a native file served by a route (#455): fetch with Range, and a <video> that seeks.
describe('HTTP byte ranges', () => {
    const url = '/webfront/services/test-media/sound.wav';
    const total = 44 + 8000 * 20;

    const bytesOf = async response => new Uint8Array(await response.arrayBuffer());

    it('announces ranges and serves a part, a suffix and an open range', async () => {
        const whole = await fetch(url);
        expect(whole.status).toBe(200);
        expect(whole.headers.get('accept-ranges')).toBe('bytes');
        const all = await bytesOf(whole);
        expect(all.length).toBe(total);

        const part = await fetch(url, {headers: {Range: 'bytes=100-199'}});
        expect(part.status).toBe(206);
        expect(part.headers.get('content-range')).toBe(`bytes 100-199/${total}`);
        expect(await bytesOf(part)).toEqual(all.slice(100, 200));

        const suffix = await fetch(url, {headers: {Range: 'bytes=-50'}});
        expect(suffix.status).toBe(206);
        expect(await bytesOf(suffix)).toEqual(all.slice(total - 50));

        const open = await fetch(url, {headers: {Range: `bytes=${total - 10}-`}});
        expect(open.status).toBe(206);
        expect(await bytesOf(open)).toEqual(all.slice(total - 10));
    });

    it('answers 416 past the end, in full for several ranges, and honours If-Range', async () => {
        const past = await fetch(url, {headers: {Range: `bytes=${total}-`}});
        expect(past.status).toBe(416);
        expect(past.headers.get('content-range')).toBe(`bytes */${total}`);

        const several = await fetch(url, {headers: {Range: 'bytes=0-1,5-6'}});
        expect(several.status).toBe(200);
        expect((await bytesOf(several)).length).toBe(total);

        const tag = (await fetch(url, {method: 'HEAD'})).headers.get('etag');
        expect(tag).toBeTruthy();
        const matching = await fetch(url, {headers: {Range: 'bytes=0-9', 'If-Range': tag}});
        expect(matching.status).toBe(206);
        const stale = await fetch(url, {headers: {Range: 'bytes=0-9', 'If-Range': '"stale"'}});
        expect(stale.status).toBe(200);
        expect((await bytesOf(stale)).length).toBe(total);
    });

    it('lets a <video> seek into the middle of the resource and resume', async () => {
        // Only the requests the video makes count: the explicit fetches above also asked for ranges.
        const before = (await webFront.cppFunction('mediaRanges')()).split('\n').length;
        const video = document.createElement('video');
        video.preload = 'auto';
        video.muted = true;
        const once = name => new Promise((resolve, reject) => {
            video.addEventListener(name, resolve, {once: true});
            video.addEventListener('error', () => reject(new Error(`video error ${video.error?.code}: ${video.error?.message}`)), {once: true});
        });
        try {
            const ready = once('loadedmetadata');
            video.src = url;
            await ready;
            expect(video.duration).toBeGreaterThan(19);
            const seeked = once('seeked');
            video.currentTime = video.duration / 2;
            await seeked;
            expect(video.currentTime).toBeGreaterThan(9);
            const playing = once('timeupdate');
            await video.play();
            await playing;
            video.pause();
            const seekedAgain = once('seeked');
            video.currentTime = 2;
            await seekedAgain;
            expect(video.currentTime).toBeLessThan(5);
        } finally {
            video.removeAttribute('src');
            video.load();
        }
        // Only the requests of the video count (the explicit fetches above are before the baseline). What a media
        // engine asks is its own business: WebKit probes with a range then buffers the whole resource, GStreamer
        // (WebKitGTK) may download it without any Range. The ranges themselves are checked by the specs above; here
        // the video must have loaded this resource from the route and positioned itself in it.
        const requests = (await webFront.cppFunction('mediaRanges')()).split('\n').slice(before - 1).filter(range => range !== '');
        expect(requests.length).toBeGreaterThan(0);
    });
});

// HTTP reads bound to the browser client of the connection (#469): token and cookie, neutralized active content.
describe('Bound HTTP reads', () => {
    const readUrl = name => webFront.cppFunction('readUrl')(name);
    const settle = ms => new Promise(resolve => setTimeout(resolve, ms));

    it('binds this browser: the cookie exchange completes', async () => {
        await webFront.bound;
    });

    it('reads a granted URL with the cookie and neutralizing headers', async () => {
        await webFront.bound;
        const response = await fetch(await readUrl('hello.txt'));
        expect(response.status).toBe(200);
        expect(await response.text()).toBe('hello');
        expect(response.headers.get('cache-control')).toBe('no-store');
        expect(response.headers.get('referrer-policy')).toBe('no-referrer');
        expect(response.headers.get('x-content-type-options')).toBe('nosniff');
        expect(response.headers.get('content-security-policy')).toBe("sandbox; default-src 'none'");
        expect(response.headers.get('cross-origin-resource-policy')).toBe('same-origin');
    });

    it('refuses the same URL without the cookie, as another client would', async () => {
        await webFront.bound;
        const url = await readUrl('hello.txt');
        expect((await fetch(url, {credentials: 'omit'})).status).toBe(403);
        expect((await fetch(url)).status).toBe(200);
        const altered = url.replace(/[0-9a-f](\/[^/]*)$/, match => (match[0] === 'a' ? 'b' : 'a') + match.slice(1));
        expect((await fetch(altered)).status).toBe(403);
    });

    it('announces HTML as an octet-stream download and keeps an SVG type', async () => {
        await webFront.bound;
        const html = await fetch(await readUrl('evil.html'));
        expect(html.headers.get('content-type')).toBe('application/octet-stream');
        expect(html.headers.get('content-disposition')).toContain('attachment');
        const svg = await fetch(await readUrl('evil.svg'));
        expect(svg.headers.get('content-type')).toBe('image/svg+xml');
        expect(svg.headers.get('content-disposition')).toContain('attachment');
    });

    it('displays a granted SVG by <img> without running its script', async () => {
        await webFront.bound;
        const image = new Image();
        const loaded = new Promise((resolve, reject) => { image.onload = resolve; image.onerror = () => reject(new Error('the SVG did not load')); });
        image.src = await readUrl('evil.svg');
        await loaded;
        await settle(300);
        expect(window.__readExecuted).toBeUndefined();
    });

    it('does not execute hostile HTML opened in a frame', async () => {
        await webFront.bound;
        const frame = document.createElement('iframe');
        frame.src = await readUrl('evil.html');
        document.body.appendChild(frame);
        try {
            await settle(700);
            expect(window.__readExecuted).toBeUndefined();
        } finally {
            frame.remove();
        }
    });

    it('does not execute a hostile SVG opened in a frame', async () => {
        await webFront.bound;
        const frame = document.createElement('iframe');
        frame.src = await readUrl('evil.svg');
        document.body.appendChild(frame);
        try {
            await settle(700);
            expect(window.__readExecuted).toBeUndefined();
        } finally {
            frame.remove();
        }
    });

    it('does not run a read announced with a legacy JavaScript type as a script', async () => {
        await webFront.bound;
        for (const name of ['legacy.js', 'legacy-ecma.js']) {
            const script = document.createElement('script');
            script.src = await readUrl(name);
            document.head.appendChild(script);
            await settle(700);
            script.remove();
            expect(window.__readExecuted).toBeUndefined();
        }
    });

    it('sends the binding exchange to the application origin whatever <base href> says', async () => {
        const base = document.createElement('base');
        base.href = 'https://cdn.example.invalid/assets/';
        document.head.prepend(base);
        const realFetch = window.fetch;
        const seen = [];
        window.fetch = (input, init) => { seen.push({url: String(input), init}); return Promise.reject(new Error('stubbed')); };
        try {
            const bridge = new WebFrontBridge('ws://127.0.0.1:1');
            bridge.boundPromise.catch(() => {});
            await bridge.bindClient('0'.repeat(64));
        } finally {
            window.fetch = realFetch;
            base.remove();
        }
        expect(seen.length).toBe(1);
        const target = new URL(seen[0].url, base.href);  // resolved against the hostile base, as the browser would
        expect(target.origin).toBe(window.location.origin);
        expect(target.pathname.startsWith('/webfront/services/bind/')).toBeTrue();
        expect(seen[0].init.mode).toBe('same-origin');
        expect(seen[0].init.redirect).toBe('error');
    });

    it('derives the WebSocket address from the protocol of the page', () => {
        expect(WebFrontBridge.defaultUrl({protocol: 'http:', host: 'localhost:9002'})).toBe('ws://localhost:9002');
        expect(WebFrontBridge.defaultUrl({protocol: 'https:', host: 'app.example:443'})).toBe('wss://app.example:443');
        expect(WebFrontBridge.defaultUrl()).toBe(`${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.host}`);
    });

    it('rejects bound when the initial connection fails', async () => {
        const bridge = new WebFrontBridge('ws://127.0.0.1:1');
        await expectAsync(bridge.bound).toBeRejected();
    });
});

// C++ also checks what it observed: it is told over this connection, after every call the specs sent.
afterAll(async () => { await webFront.cppFunction('specsFinished')(); });

await notifyBrowserReady();
