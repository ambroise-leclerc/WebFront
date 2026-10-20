import './content-policy-specs.mjs';
import { importedValue } from './module-fixture.mjs';
import './terminal-specs.mjs';
import './tools-specs.mjs';
import './react-demo-specs.mjs';

describe('Frontend page URL', () => {
    it('opens a filename containing spaces and a literal hash', () => {
        expect(decodeURIComponent(location.pathname)).toBe('/encoded path #.html');
    });
    it('serves a filename containing a literal plus through the C++ URL builder', async () => {
        const url = await webFront.cppFunction('plusPageUrl')();
        expect(new URL(url).pathname).toBe('/a%2Bb.html');
        const response = await fetch(url);
        expect(response.ok).toBeTrue();
        expect(await response.text()).toContain('<title>WebFront automated browser tests</title>');
    });
    it('preserves the query and fragment passed by C++', () => {
        expect(location.search).toBe('?webfront=1');
        expect(location.hash).toBe('#integration');
        expect(location.hostname).toBe('127.0.0.1');
    });
});

const cppToken = 'cpp-to-js-token';
const jsToken = 'js-to-cpp-token';
const status = document.getElementById('bridge-status');

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

let finishBitsetChecks;
const bitsetChecks = new Promise(resolve => { finishBitsetChecks = resolve; });

globalThis.webfrontTests = {
    bitsetEcho(value) {
        if (!(value instanceof Uint8Array) || value.length !== 2 || value[0] !== 0x81 || value[1] !== 1)
            throw new Error('Incorrect C++ packed bitset');
        return value;
    },
    bitsetBad(kind) { return [new Uint8Array([0, 2]), new Uint8Array(1), new Int8Array(2)][kind]; },
    bitsetChecksFinished(passed) { finishBitsetChecks(passed); },
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

    returnToCpp(value) {
        const result = `js-result:${value}`;
        resolveCppResult(result);
        return result;
    },

    close(passed) {
        status.textContent = passed ? 'Bridge integration passed' : 'Bridge integration failed';
        setTimeout(() => globalThis.close(), 0);
    }
};

describe('WebFront browser integration', () => {
    it('supports packed bitsets in C++ future and sender calls and rejects malformed returns', async () => {
        await expectAsync(bitsetChecks).toBeResolvedTo(true);
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

    it('rejects missing functions and C++ exceptions', async () => {
        await expectAsync(webFront.cppFunction('missingCppFunction')())
            .toBeRejectedWithError(/was not found/);
        await expectAsync(webFront.cppFunction('throwFromCpp')())
            .toBeRejectedWithError('C++ callback failed');
    });

    it('rejects a C++ result longer than the 65535-byte string limit', async () => {
        await expectAsync(webFront.cppFunction('returnOverlongFromCpp')())
            .toBeRejectedWithError('WebFront strings cannot exceed 65535 bytes');
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

    it('sends strings up to the limit and rejects longer ones before sending', async () => {
        const lengthInCpp = webFront.cppFunction('lengthInCpp');
        await expectAsync(lengthInCpp('x'.repeat(65535))).toBeResolvedTo(65535);
        await expectAsync(lengthInCpp('x'.repeat(65536)))
            .toBeRejectedWithError(RangeError, 'WebFront strings cannot exceed 65535 UTF-8 bytes');
    });

    it('lets a C++ callback call back the browser that called it', async () => {
        await webFront.cppFunction('callBackCaller')('caller-token');
        await expectAsync(callerCall).toBeResolvedTo('caller-token');
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

jasmine.getEnv().addReporter({
    failures: [],
    specDone(result) {
        for (const failure of result.failedExpectations)
            this.failures.push(`${result.fullName}: ${failure.message}`);
    },
    async jasmineDone(result) {
        await webFront.cppFunction('reportJasmine')(result.overallStatus, this.failures.join('\n'));
    }
});

await notifyBrowserReady();
