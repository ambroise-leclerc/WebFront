import { watchOriginProbe } from '../pages/js/origin-probe.mjs';

describe('Server bridge origin authorization', () => {
    const socketUrl = new URL('/', location.href);
    socketUrl.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const controls = new Set();
    afterAll(() => {
        for (const socket of controls) socket.close();
        controls.clear();
    });

    function allowedProbe() {
        return new Promise((resolve, reject) => {
            const socket = watchOriginProbe(socketUrl.href, outcome => { clearTimeout(timeout); resolve(outcome); });
            controls.add(socket);
            const timeout = setTimeout(() => { reject(new Error('Allowed-origin probe timed out')); socket.close(); }, 5000);
        });
    }

    async function probe(kind) {
        if (kind === 'allowed') return allowedProbe();
        const iframe = document.createElement('iframe');
        const token = `origin-probe-${Math.random()}`;
        const result = new Promise((resolve, reject) => {
            const timeout = setTimeout(() => { cleanup(); reject(new Error('Origin probe timed out')); }, 5000);
            function cleanup() {
                clearTimeout(timeout);
                window.removeEventListener('message', receive);
                iframe.remove();
            }
            function receive(event) {
                if (event.source !== iframe.contentWindow || event.data?.token !== token) return;
                const outcome = event.data.outcome;
                cleanup();
                resolve(outcome);
            }
            window.addEventListener('message', receive);
        });
        if (kind === 'opaque') {
            iframe.sandbox = 'allow-scripts';
            iframe.srcdoc = `<script>
                (${watchOriginProbe.toString()})(${JSON.stringify(socketUrl.href)},
                    outcome => parent.postMessage({token: '${token}', outcome}, '*'));
            <\/script>`;
        } else {
            const pageUrl = new URL('/origin-probe.html', location.href);
            if (kind === 'foreign') pageUrl.hostname = 'localhost';
            pageUrl.searchParams.set('token', token);
            pageUrl.searchParams.set('socket', socketUrl.href);
            iframe.src = pageUrl.href;
        }
        document.body.append(iframe);
        return result;
    }

    it('serves the normal same-origin bridge', async () => {
        await webFront.ready;
        expect(await webFront.cppFunction('returnFromCpp')('origin')).toBe('cpp-result:origin');
    });
    it('rejects a real cross-origin page connecting to the local bridge', async () => {
        expect(await probe('allowed')).withContext('allowed origin before the foreign-origin probe').toBe('opened');
        expect(await probe('foreign')).toBe('rejected');
        expect(await probe('allowed')).withContext('allowed origin after the foreign-origin probe').toBe('opened');
    });
    it('rejects opaque origins from sandboxed pages', async () => {
        expect(await probe('allowed')).withContext('allowed origin before the opaque-origin probe').toBe('opened');
        expect(await probe('opaque')).toBe('rejected');
        expect(await probe('allowed')).withContext('allowed origin after the opaque-origin probe').toBe('opened');
    });
});

describe('Origin probe lifetime', () => {
    it('waits for rejected sockets to close and reports successful handshakes once', () => {
        for (const opened of [false, true]) {
            let socket;
            class FakeSocket {
                closeRequested = false;
                constructor() { socket = this; }
                close() { this.closeRequested = true; }
            }
            const outcomes = [];
            watchOriginProbe('ws://127.0.0.1/', outcome => outcomes.push(outcome), FakeSocket);
            if (opened) socket.onopen();
            else socket.onerror?.();
            expect(outcomes).toEqual(opened ? ['opened'] : []);
            expect(socket.closeRequested).toBe(false);
            socket.onclose();
            expect(outcomes).toEqual([opened ? 'opened' : 'rejected']);
        }
    });
});
