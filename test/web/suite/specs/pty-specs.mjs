import { mount } from '@webfront/pty';

describe('Native PTY page adapter', () => {
    const call = operation => webFront.cppFunction('webfront.pty.' + operation);

    function completion() {
        let resolve, reject;
        const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
        return {promise, resolve, reject};
    }

    function xtermProbe() {
        const decoder = new TextDecoder();
        return {
            text: '',
            cols: 87,
            rows: 29,
            loadAddon() {},
            open() {},
            onData(callback) {
                this.send = callback;
                return {dispose: () => { this.send = undefined; }};
            },
            onBinary() { return {dispose() {}}; },
            reset() { this.text = ''; },
            write(bytes, done) {
                this.text += decoder.decode(bytes, {stream: true});
                done();
            },
        };
    }

    it('reads one terminal byte stream across split UTF-8 sequences', async () => {
        const terminal = xtermProbe();
        const bridge = {
            ready: Promise.resolve(),
            cppFunction(name) {
                if (!name.endsWith('.read')) return async () => {};
                return async offset => [
                    [1, '4g==', false, false, 0],
                    [2, 'gg==', false, false, 0],
                    [3, 'rA==', false, true, 42],
                ][offset];
            },
        };
        const element = document.createElement('div');
        const exit = completion();
        const session = await mount(element, {terminal, fit: {fit() {}}, webFront: bridge, onExit: exit.resolve, onError: exit.reject});
        try {
            expect(await exit.promise).toBe(42);
            expect(terminal.text).toBe('€');
        } finally {
            session.dispose();
        }
    });

    it('drives a native shell, rejects an oversized input and replays after reattachment', async () => {
        expect(await call('open')()).toBeUndefined();
        expect(await call('write')(new Uint8Array(4 * 1024 * 1024))).toBeFalse();
        expect(await webFront.cppFunction('returnFromCpp')('pty')).toBe('cpp-result:pty');
        const element = document.createElement('div');
        const terminal = xtermProbe();
        const exit = completion();
        const session = await mount(element, {terminal, fit: {fit() {}}, onExit: exit.resolve, onError: exit.reject});
        try {
            // Both cmd and sh accept these commands with carriage return in their native terminal.
            terminal.send('echo WEBFRONT_PTY_OK\r');
            terminal.send('exit 42\r');
            expect(await exit.promise).toBe(42);
            expect(terminal.text).toMatch(/\nWEBFRONT_PTY_OK(?:\r?\n|$)/);
        } finally {
            session.dispose();
        }

        const replay = xtermProbe();
        const replayExit = completion();
        const reattached = await mount(element, {terminal: replay, fit: {fit() {}}, onExit: replayExit.resolve, onError: replayExit.reject});
        try {
            expect(await replayExit.promise).toBe(42);
            expect(replay.text).toBe(terminal.text);
        } finally {
            reattached.dispose();
        }
    });
});
