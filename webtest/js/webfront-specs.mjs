import { importedValue } from './module-fixture.mjs';

const cppToken = 'cpp-to-js-token';
const jsToken = 'js-to-cpp-token';
const status = document.getElementById('bridge-status');

async function notifyBrowserReady() {
    for (;;) {
        try {
            await webFront.cppFunction('browserReady')();
            return;
        } catch (error) {
            if (!(error instanceof Error) || error.message !== 'WebFront bridge is not connected')
                throw error;
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

let resolveCppResult;
const cppResultCall = new Promise((resolve) => {
    resolveCppResult = resolve;
});

globalThis.webfrontTests = {
    receiveFromCpp(token) {
        status.textContent = `C++ called served JavaScript with: ${token}`;
        resolveCppCall(token);
    },

    receiveArraysFromCpp(...arrays) {
        resolveCppArraysCall(arrays);
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

    it('allows C++ to await a JavaScript result', async () => {
        await expectAsync(cppResultCall).toBeResolvedTo('js-result:from-cpp');
    });
});

jasmine.getEnv().addReporter({
    failures: [],
    specDone(result) {
        for (const failure of result.failedExpectations)
            this.failures.push(`${result.fullName}: ${failure.message}`);
    },
    jasmineDone(result) {
        webFront.cppFunction('reportJasmine')(result.overallStatus, this.failures.join('\n'));
    }
});

await notifyBrowserReady();
