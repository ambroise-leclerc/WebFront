import { importedValue } from './module-fixture.mjs';

const cppToken = 'cpp-to-js-token';
const jsToken = 'js-to-cpp-token';
const status = document.getElementById('bridge-status');

let resolveCppCall;
const cppCall = new Promise((resolve) => {
    resolveCppCall = resolve;
});

globalThis.webfrontTests = {
    receiveFromCpp(token) {
        status.textContent = `C++ called served JavaScript with: ${token}`;
        resolveCppCall(token);
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
});

jasmine.getEnv().addReporter({
    jasmineDone(result) {
        webFront.cppFunction('reportJasmine')(result.overallStatus);
    }
});

webFront.cppFunction('browserReady')();
