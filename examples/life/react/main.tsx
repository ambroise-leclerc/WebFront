import React, {StrictMode, useEffect, useRef} from 'react';
import {createRoot} from 'react-dom/client';
import {Island, createStore, useStore} from '@webfront/react';
// The optional interpreter is served by WebFront, not bundled into the adapter.
const interpreterUrl = '/webfront-dom.mjs';

declare const webFront: {ready: Promise<void>; cppFunction(name: string): (...args: unknown[]) => Promise<unknown>};
async function main() {
    const {installDomInterpreter} = await import(interpreterUrl);
    const dom = installDomInterpreter(webFront);
    const store = createStore<[Uint8Array, number, number]>('life', [new Uint8Array(128), 0, 0]);
    function Life({started}: {started: boolean}) {
        const [cells, generation, population] = useStore(store);
        const canvas = useRef<HTMLCanvasElement>(null);
        useEffect(() => {
            const context = canvas.current!.getContext('2d')!;
            for (let i = 0; i < 1024; ++i) {
                context.fillStyle = cells[i >> 3] & (1 << (i & 7)) ? '#81efbd' : '#16362c';
                context.fillRect((i % 32) * 12, (i >> 5) * 12, 11, 11);
            }
        }, [cells]);
        return <main style={{fontFamily: 'sans-serif', padding: 24, background: '#091d17', color: '#dcffed'}}>
            <h1>Game of Life · React + C++ island</h1>
            <p data-testid="react-status">Generation {generation} · {population} living cells</p>
            <canvas ref={canvas} width={384} height={384} aria-disabled={!started} aria-label="Game of Life; click a cell to toggle it"
                onClick={event => {
                    if (!started) return;
                    const rect = event.currentTarget.getBoundingClientRect();
                    const x = Math.floor((event.clientX - rect.left) * 32 / rect.width);
                    const y = Math.floor((event.clientY - rect.top) * 32 / rect.height);
                    void webFront.cppFunction('life.react.toggle')(y * 32 + x).catch(showError);
                }}/>
            <Island dom={dom} name="life-panel" data-testid="cpp-panel"/>
            <p id="error" role="alert"/>
        </main>;
    }
    function showError(error: unknown) { document.getElementById('error')!.textContent = String(error); }
    const root = createRoot(document.getElementById('root')!);
    try {
        root.render(<StrictMode><Life started={false}/></StrictMode>);
        // Wait for the committed Island before lending it to C++; store reception is already installed.
        const deadline = performance.now() + 5000;
        while (!document.querySelector('[data-testid="cpp-panel"]')) {
            if (performance.now() >= deadline) throw new Error('Life panel did not mount within 5 seconds');
            await new Promise(resolve => setTimeout(resolve, 16));
        }
        await dom.connect();
        await webFront.cppFunction('life.react.start')();
        root.render(<StrictMode><Life started={true}/></StrictMode>);
    } catch (error) {
        root.unmount();
        store.dispose();
        throw error;
    }
}
void main().catch(error => { document.body.textContent = String(error); });
