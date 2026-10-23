import * as React from 'react';
import { mountSignalStudio } from '../react-demo/SignalStudio.jsx';
import '../react-demo/style.css';

// The page of the Signal Studio example, built by esbuild-wasm with the React of the lock, rendered with the real bridge.
describe('The React signal example', () => {
    let container;
    let page;

    async function waitFor(predicate) {
        const deadline = Date.now() + 4000;
        while (!predicate()) {
            if (Date.now() > deadline)
                throw new Error('React example did not reach the expected state');
            await new Promise(resolve => setTimeout(resolve, 10));
        }
    }

    beforeEach(async () => {
        container = document.createElement('div');
        document.body.appendChild(container);
        page = mountSignalStudio(container, webFront);
        await waitFor(() => container.querySelector('.app')?.dataset.state === 'ready');
    });

    afterEach(() => {
        page?.unmount();
        container?.remove();
    });

    it('renders real C++ samples using the React of the lock', () => {
        expect(React.version).toBe('19.3.1');
        expect(container.querySelector('h1').textContent).toBe('Signal Studio');
        expect(container.querySelector('.connection').textContent).toContain('C++ engine connected');
        expect(container.querySelector('.plot-footer').textContent).toContain('256 samples');
        expect(Number.parseFloat(container.querySelector('#rms').textContent)).toBeCloseTo(0.570, 3);
        expect(container.querySelector('#signal-path').getAttribute('d').match(/L/g).length).toBe(255);
    });

    it('regenerates the waveform when a parameter changes', async () => {
        const initial = container.querySelector('#signal-path').getAttribute('d');
        const frequency = container.querySelector('#frequency');
        // Use the native setter, allowing React to observe the change rather than its value tracker.
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(frequency, '7');
        frequency.dispatchEvent(new Event('input', {bubbles: true}));
        await waitFor(() => container.querySelector('output[for="frequency"]').textContent.includes('7 Hz'));
        container.querySelector('.run').click();
        await waitFor(() => container.querySelector('#signal-path').getAttribute('d') !== initial);
        expect(container.querySelector('#signal-path').getAttribute('d')).not.toBe(initial);
        expect(container.querySelector('.app').dataset.state).toBe('ready');
    });

    it('handles a rejected call and allows another attempt', async () => {
        const initial = container.querySelector('#signal-path').getAttribute('d');
        page.render({
            ready: webFront.ready,
            // Deliberately pass an invalid frequency to the real C++ callback.
            cppFunction: name => (_frequency, amplitude, harmonics) => webFront.cppFunction(name)(0, amplitude, harmonics),
        });
        await waitFor(() => container.querySelector('.run') !== null);
        container.querySelector('.run').click();
        await waitFor(() => container.querySelector('.app').dataset.state === 'error');
        expect(container.querySelector('[role="alert"]').textContent).toContain('outside the supported range');
        expect(container.querySelector('.run').disabled).toBeFalse();
        expect(container.querySelector('#signal-path').getAttribute('d')).toBe(initial);
        page.render(webFront);
        await waitFor(() => container.querySelector('.run') !== null);
        container.querySelector('.run').click();
        await waitFor(() => container.querySelector('.app').dataset.state === 'ready');
        expect(container.querySelector('[role="alert"]')).toBeNull();
    });
});
