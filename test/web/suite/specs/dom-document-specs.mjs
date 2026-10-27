// Separate real connections exercise the per-browser C++ Document, including a reload.
async function until(predicate) {
    const deadline = performance.now() + 5000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error('DOM document condition timed out');
        await new Promise(resolve => setTimeout(resolve, 5));
    }
}
async function connectedFrame(defer = false) {
    const frame = document.createElement('iframe');
    const loaded = new Promise((resolve, reject) => { frame.onload = resolve; frame.onerror = reject; });
    frame.src = defer ? '/dom-document.html?defer' : '/dom-document.html';
    document.body.appendChild(frame);
    await loaded;
    await until(() => !!frame.contentWindow.documentConnected);
    await frame.contentWindow.documentConnected;
    return frame;
}
function call(frame, name, ...args) { return frame.contentWindow.webFront.cppFunction(name)(...args); }

describe('C++ DOM document API', () => {
    it('rejects the first off-thread document access without binding the document to that thread', async () => {
        const frame = await connectedFrame(true);
        try {
            expect(await call(frame, 'domDocumentOffThread')).toBeTrue();
            await frame.contentWindow.connectDocument();
            await call(frame, 'wfDom.ready'); // Idempotent if a prior readiness acknowledgement was lost.
            const id = await call(frame, 'domDocumentBuild');
            expect(await call(frame, 'domDocumentNodeValid', id)).toBeTrue();
        } finally { frame.remove(); }
    });
    it('retries readiness when C++ accepted it but the acknowledgement was lost', async () => {
        const frame = await connectedFrame(true);
        const page = frame.contentWindow;
        const original = page.webFront.cppFunction;
        let loseAcknowledgement = true;
        page.webFront.cppFunction = function(name) {
            const invoke = original.call(this, name);
            return (...args) => invoke(...args).then(value => {
                if (name === 'wfDom.ready' && loseAcknowledgement) {
                    loseAcknowledgement = false;
                    throw new Error('readiness acknowledgement lost');
                }
                return value;
            });
        };
        try {
            await page.connectDocument().then(
                () => { throw new Error('The first acknowledgement should be lost'); },
                error => { expect(error.message).toBe('readiness acknowledgement lost'); });
            await page.connectDocument();
            const id = await call(frame, 'domDocumentBuild');
            expect(await call(frame, 'domDocumentNodeValid', id)).toBeTrue();
        } finally { page.webFront.cppFunction = original; frame.remove(); }
    });

    it('batches writes automatically, handles event flags and groups explicit sender reads', async () => {
        const frame = await connectedFrame();
        try {
            const page = frame.contentWindow, root = page.document.getElementById('root');
            const id = await call(frame, 'domDocumentBuild');
            await until(() => root.querySelector('input'));
            expect(root.firstChild.nodeType).toBe(Node.TEXT_NODE);
            expect(root.firstChild.textContent).toBe('C++ été 🌍');
            let bubbled = false;
            root.addEventListener('input', () => { bubbled = true; });
            const input = root.querySelector('input');
            input.value = 'changed 🌍';
            const event = new page.Event('input', {bubbles: true, cancelable: true});
            input.dispatchEvent(event);
            expect(event.defaultPrevented).toBeTrue();
            expect(bubbled).toBeFalse();
            await until(() => root.firstChild.textContent === input.value);
            const result = new Promise(resolve => { page.documentReadResult = resolve; });
            await call(frame, 'domDocumentRead');
            expect(await result).toBe('changed 🌍|changed 🌍');

            // Cancellation ends the local wait. Resolve the browser Promise afterwards to exercise a late reply.
            let late;
            const original = page.wfDom.read;
            page.wfDom.read = () => new Promise(resolve => { late = resolve; });
            await call(frame, 'domDocumentRead');
            await until(() => late);
            await call(frame, 'domDocumentCancelRead');
            await until(() => call(frame, 'domDocumentCancelled'));
            late(new page.Uint8Array(0));
            page.wfDom.read = original;
            await call(frame, 'domDocumentCancelled'); // Round trip after the late reply.
            page.releaseIsland();
            expect(await call(frame, 'domDocumentNodeValid', id)).toBeFalse();
        } finally { frame.remove(); }
    });

    it('isolates browsers and invalidates handles on disconnect and reload', async () => {
        const first = await connectedFrame(), second = await connectedFrame();
        try {
            const firstId = await call(first, 'domDocumentBuild');
            const secondId = await call(second, 'domDocumentBuild');
            expect(firstId).not.toBe(secondId);
            first.remove();
            await until(async () => !await webFront.cppFunction('domDocumentNodeValid')(firstId));
            expect(await webFront.cppFunction('domClosedDocumentRejected')(firstId)).toBeTrue();
            expect(await call(second, 'domDocumentNodeValid', secondId)).toBeTrue();
            const loaded = new Promise(resolve => { second.onload = resolve; });
            second.contentWindow.location.reload();
            await loaded;
            await until(() => !!second.contentWindow.documentConnected);
            await second.contentWindow.documentConnected;
            const newId = await call(second, 'domDocumentBuild');
            expect(newId).not.toBe(secondId);
            expect(await call(second, 'domDocumentNodeValid', secondId)).toBeFalse();
            expect(await call(second, 'domDocumentNodeValid', newId)).toBeTrue();
        } finally { first.remove(); second.remove(); }
    });
});
