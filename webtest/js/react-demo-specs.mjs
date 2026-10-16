describe('The embedded React signal example', () => {
    let frame;
    let doc;

    async function waitFor(predicate) {
        const deadline = Date.now() + 8000;
        while (!predicate()) {
            if (Date.now() > deadline)
                throw new Error('React example did not reach the expected state');
            await new Promise(resolve => setTimeout(resolve, 10));
        }
    }

    beforeEach(async () => {
        const response = await fetch('/demos/react.html');
        if (!response.ok)
            throw new Error(`Could not load React example: ${response.status}`);
        const html = await response.text();
        frame = document.createElement('iframe');
        frame.style.cssText = 'width:1100px;height:720px;border:0';
        // Reuse the runner's real C++ connection; a second UI would replace its callback target.
        frame.srcdoc = html.replace('<script src="/WebFront.js"></script>',
            '<script>window.webFront = parent.webFront;</script>');
        document.body.appendChild(frame);
        await waitFor(() => frame.contentDocument?.querySelector('.app')?.dataset.state === 'ready');
        doc = frame.contentDocument;
    });

    afterEach(() => frame?.remove());

    it('renders real C++ samples using the embedded React and Babel assets', () => {
        expect(doc.querySelector('h1').textContent).toBe('Signal Studio');
        expect(doc.querySelector('.connection').textContent).toContain('C++ engine connected');
        expect(doc.querySelector('.plot-footer').textContent).toContain('256 samples');
        expect(Number.parseFloat(doc.querySelector('#rms').textContent)).toBeCloseTo(0.570, 3);
        expect(doc.querySelector('#signal-path').getAttribute('d').match(/L/g).length).toBe(255);
        const resources = frame.contentWindow.performance.getEntriesByType('resource');
        expect(resources.some(entry => entry.name.includes('/react@18/'))).toBeTrue();
        expect(resources.every(entry => new URL(entry.name).origin === location.origin)).toBeTrue();
    });

    it('regenerates the waveform when a parameter changes', async () => {
        const initial = doc.querySelector('#signal-path').getAttribute('d');
        const frequency = doc.querySelector('#frequency');
        // Use the native setter, allowing React to observe the change rather than its value tracker.
        Object.getOwnPropertyDescriptor(frame.contentWindow.HTMLInputElement.prototype, 'value').set.call(frequency, '7');
        frequency.dispatchEvent(new frame.contentWindow.Event('input', {bubbles: true}));
        await waitFor(() => doc.querySelector('output[for="frequency"]').textContent.includes('7 Hz'));
        doc.querySelector('.run').click();
        await waitFor(() => doc.querySelector('#signal-path').getAttribute('d') !== initial);
        expect(doc.querySelector('#signal-path').getAttribute('d')).not.toBe(initial);
        expect(doc.querySelector('.app').dataset.state).toBe('ready');
    });

    it('handles a rejected call and allows another attempt', async () => {
        const initial = doc.querySelector('#signal-path').getAttribute('d');
        frame.contentWindow.webFront = {
            ready: webFront.ready,
            // Deliberately pass an invalid frequency to the real C++ callback.
            cppFunction: name => (_frequency, amplitude, harmonics) => webFront.cppFunction(name)(0, amplitude, harmonics),
        };
        doc.querySelector('.run').click();
        await waitFor(() => doc.querySelector('.app').dataset.state === 'error');
        expect(doc.querySelector('[role="alert"]').textContent).toContain('outside the supported range');
        expect(doc.querySelector('.run').disabled).toBeFalse();
        expect(doc.querySelector('#signal-path').getAttribute('d')).toBe(initial);
        frame.contentWindow.webFront = webFront;
        doc.querySelector('.run').click();
        await waitFor(() => doc.querySelector('.app').dataset.state === 'ready');
        expect(doc.querySelector('[role="alert"]')).toBeNull();
    });
});
