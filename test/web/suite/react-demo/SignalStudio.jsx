// The Signal Studio example page as a module: the spec renders it with the React of its lock.
import { useState, useEffect } from "react";
import { flushSync } from "react-dom";
import { createRoot } from "react-dom/client";

function Control({ id, label, value, min, max, step, unit, onChange, disabled }) {
    return <div className="control">
        <label htmlFor={id}>{label}<output htmlFor={id}>{value.toFixed(step < 1 ? 2 : 0)} {unit}</output></label>
        <input id={id} type="range" min={min} max={max} step={step} value={value} disabled={disabled}
            onChange={event => onChange(Number(event.target.value))} />
        <div className="range-limits"><span>{min} {unit}</span><span>{max} {unit}</span></div>
    </div>;
}

function App({ bridge }) {
    const [frequency, setFrequency] = useState(4);
    const [amplitude, setAmplitude] = useState(0.8);
    const [harmonics, setHarmonics] = useState(0.35);
    const [samples, setSamples] = useState([]);
    const [busy, setBusy] = useState(true);
    const [error, setError] = useState('');

    async function generate() {
        setBusy(true);
        setError('');
        try {
            await bridge.ready;
            const values = await bridge.cppFunction('signal.generate')(frequency, amplitude, harmonics);
            setSamples(Array.from(values));
        } catch (failure) {
            setError(`Could not generate signal: ${failure.message}`);
        } finally {
            setBusy(false);
        }
    }

    useEffect(() => { generate(); }, []);

    const peak = samples.reduce((maximum, value) => Math.max(maximum, Math.abs(value)), 0);
    const rms = samples.length ? Math.sqrt(samples.reduce((sum, value) => sum + value * value, 0) / samples.length) : 0;
    const plotRange = Math.max(1, peak * 1.2);
    const path = samples.map((value, index) => `${index ? 'L' : 'M'}${(index / (samples.length - 1) * 640).toFixed(2)},${(120 - value / plotRange * 100).toFixed(2)}`).join(' ');

    return <div className="app" data-state={busy ? 'busy' : error ? 'error' : 'ready'}>
        <aside>
            <div className="brand"><span className="brand-mark" />WebFront</div>
            <div className="aside-label">Workspace</div>
            <div className="nav-current"><span>∿</span>Signal studio</div>
            <div className="aside-bottom"><strong>A little C++. A complete GUI.</strong><br />React 18 · C++23<br />All assets served locally.</div>
        </aside>
        <main>
            <header>
                <div><p className="eyebrow">The C++ workbench</p><h1>Signal Studio</h1><p className="intro">Shape a waveform. Let C++ do the maths.</p></div>
                <span className="connection" role="status"><span className="dot" />{samples.length ? 'C++ engine connected' : 'Connecting to C++…'}</span>
            </header>
            <div className="workspace">
                <section>
                    <div className="plot-card">
                        <div className="panel-heading"><h2>Waveform</h2><span className="legend"><i />Generated signal</span></div>
                        <div className="plot">
                            <svg viewBox="-12 -8 664 256" preserveAspectRatio="none" role="img" aria-label="Signal samples calculated in C++">
                                <defs><pattern id="grid" width="64" height="48" patternUnits="userSpaceOnUse"><path d="M 64 0 L 0 0 0 48" fill="none" stroke="#294238" strokeWidth="0.8" /></pattern></defs>
                                <rect width="640" height="240" fill="url(#grid)" />
                                <line x1="0" y1="120" x2="640" y2="120" stroke="#496052" strokeDasharray="3 5" />
                                <path id="signal-path" d={path} fill="none" stroke="#dfa777" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round" />
                            </svg>
                            <div className="axis"><span>0 ms</span><span>250 ms</span><span>500 ms</span><span>750 ms</span><span>1,000 ms</span></div>
                        </div>
                        <div className="plot-footer"><span>Time domain / 1 second</span><span>{samples.length} samples · Float64</span></div>
                    </div>
                    <div className="metrics">
                        <div className="metric"><div className="metric-label">Peak amplitude</div><div className="metric-value" id="peak">{peak.toFixed(3)}<small>V</small></div></div>
                        <div className="metric"><div className="metric-label">RMS level</div><div className="metric-value" id="rms">{rms.toFixed(3)}<small>V</small></div></div>
                        <div className="metric"><div className="metric-label">Sample buffer</div><div className="metric-value">{samples.length}<small>points</small></div></div>
                    </div>
                </section>
                <section className="settings" aria-label="Signal parameters">
                    <h2>Make it your own</h2><p className="small">Adjust the parameters, then send<br />them to the C++ engine.</p>
                    <Control id="frequency" label="Frequency" value={frequency} min={1} max={12} step={1} unit="Hz" onChange={setFrequency} disabled={busy} />
                    <Control id="amplitude" label="Amplitude" value={amplitude} min={0.1} max={1.5} step={0.05} unit="V" onChange={setAmplitude} disabled={busy} />
                    <Control id="harmonics" label="Harmonic mix" value={harmonics} min={0} max={1} step={0.05} unit="" onChange={setHarmonics} disabled={busy} />
                    <button className="run" onClick={generate} disabled={busy}>{busy ? 'Calculating…' : 'Generate signal'} <span aria-hidden="true">↗</span></button>
                    <p className="settings-foot">Ordinary C++ functions. Real results.</p>
                </section>
            </div>
            {error && <p className="error" role="alert">{error}</p>}
            <div className="note"><span className="note-icon">&lt;/&gt;</span><div><h2>Your application stays C++.</h2><p>The waveform comes from a C++ callback as a typed array. React takes care of the view.<br /><code>app.cppFunction("signal.generate", &amp;demo::generateSignal);</code></p></div></div>
            <footer><span>WEBFRONT / REACT EXAMPLE</span><span>Local computation · No CDN · No build step for JavaScript</span></footer>
        </main>
    </div>;
}

/** Renders the page into `container`; render(bridge) of the result shows it again with another bridge. */
export function mountSignalStudio(container, bridge) {
    const root = createRoot(container);
    // Synchronous: the next click already uses the new bridge.
    const render = current => flushSync(() => root.render(<App bridge={current} />));
    render(bridge);
    return { render, unmount: () => root.unmount() };
}
