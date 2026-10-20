import React, {useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import './style.css';

declare const webFront: {ready: Promise<void>; cppFunction(name: string): (grid: string) => Promise<string>};
const size = 32;
const empty = () => '0'.repeat(size * size);
const shapes: Record<string, number[][]> = {
    Glider: [[1, 0], [2, 1], [0, 2], [1, 2], [2, 2]],
    Blinker: [[0, 1], [1, 1], [2, 1]],
    Galaxy: [[1, 0], [2, 0], [0, 1], [1, 1], [1, 2]],
};
function seed(name: string) {
    const cells = empty().split('');
    const offsets = name === 'Galaxy' ? [[7, 7], [21, 9], [13, 22]] : [[14, 14]];
    for (const [ox, oy] of offsets)
        for (const [x, y] of shapes[name]) cells[(y + oy) * size + x + ox] = '1';
    return cells.join('');
}

function Board({cells, flat, toggle}: {cells: string; flat: boolean; toggle: (index: number) => void}) {
    const canvas = useRef<HTMLCanvasElement>(null);
    useEffect(() => {
        const element = canvas.current!;
        function draw() {
            const ratio = window.devicePixelRatio || 1;
            const {width, height} = element.getBoundingClientRect();
            element.width = Math.round(width * ratio); element.height = Math.round(height * ratio);
            const context = element.getContext('2d')!;
            context.scale(ratio * width / 960, ratio * height / 680);
            const tile = (points: number[][], fill: string, stroke?: string) => {
                context.beginPath(); points.forEach(([x, y], i) => i ? context.lineTo(x, y) : context.moveTo(x, y));
                context.closePath(); context.fillStyle = fill; context.fill();
                if (stroke) { context.strokeStyle = stroke; context.lineWidth = .7; context.stroke(); }
            };
            for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
                const alive = cells[y * size + x] === '1';
                if (flat) {
                    context.fillStyle = alive ? '#bcf8df' : 'rgba(159,202,194,.07)';
                    context.fillRect(209 + x * 17, 65 + y * 17, 15, 15);
                    continue;
                }
                const px = 480 + (x - y) * 13, py = 115 + (x + y) * 7;
                const lift = alive ? 20 : 0;
                tile([[px, py - lift], [px + 12, py + 6 - lift], [px, py + 12 - lift], [px - 12, py + 6 - lift]],
                    alive ? '#c8ffe4' : 'rgba(145,190,182,.075)', alive ? '#eefff8' : 'rgba(160,207,195,.12)');
                if (alive) {
                    tile([[px - 12, py + 6 - lift], [px, py + 12 - lift], [px, py + 12], [px - 12, py + 6]], '#529a83');
                    tile([[px, py + 12 - lift], [px + 12, py + 6 - lift], [px + 12, py + 6], [px, py + 12]], '#7fc5a6');
                }
            }
        }
        draw();
        const observer = new ResizeObserver(draw); observer.observe(element);
        return () => observer.disconnect();
    }, [cells, flat]);
    return <canvas ref={canvas} role="img" aria-label="32 by 32 Game of Life. Use the pattern buttons and Step to explore."
        onClick={event => {
            const rect = event.currentTarget.getBoundingClientRect();
            const px = (event.clientX - rect.left) * 960 / rect.width;
            const py = (event.clientY - rect.top) * 680 / rect.height;
            const x = flat ? Math.floor((px - 209) / 17) : Math.floor(((px - 480) / 13 + (py - 115) / 7) / 2);
            const y = flat ? Math.floor((py - 65) / 17) : Math.floor(((py - 115) / 7 - (px - 480) / 13) / 2);
            if (x >= 0 && x < size && y >= 0 && y < size) toggle(y * size + x);
        }}/>;
}

function Life() {
    const [cells, setCells] = useState(() => seed('Galaxy'));
    const [generation, setGeneration] = useState(0);
    const [playing, setPlaying] = useState(false);
    const [speed, setSpeed] = useState(8);
    const [flat, setFlat] = useState(false);
    const [pattern, setPattern] = useState('Galaxy');
    const [error, setError] = useState('');
    const [busy, setBusy] = useState(false);
    const state = useRef({cells, revision: 0, pending: false, mounted: true});
    const run = useRef(playing); run.current = playing;
    const rate = useRef(speed); rate.current = speed;
    useEffect(() => { state.current.mounted = true; return () => { state.current.mounted = false; }; }, []);
    function replace(value: string, name: string) {
        run.current = false; setPlaying(false); state.current.revision++;
        state.current.cells = value; setCells(value); setGeneration(0); setPattern(name); setError('');
    }
    async function step() {
        const current = state.current;
        if (current.pending || !current.mounted) return;
        current.pending = true; setBusy(true);
        const revision = current.revision;
        try {
            await webFront.ready;
            if (!current.mounted || revision !== current.revision) return;
            const next = await webFront.cppFunction('life.step')(current.cells);
            if (!current.mounted || revision !== current.revision) return;
            if (!/^[01]{1024}$/.test(next)) throw new Error('Invalid C++ generation');
            current.cells = next; setCells(next); setGeneration(value => value + 1); setError('');
        } catch (error) {
            if (current.mounted && revision === current.revision) {
                setError(String(error)); run.current = false; setPlaying(false);
            }
        } finally { current.pending = false; if (current.mounted) setBusy(false); }
    }
    useEffect(() => {
        if (!playing) return;
        let stopped = false;
        let timer: ReturnType<typeof setTimeout>;
        async function tick() {
            if (stopped || !run.current) return;
            await step();
            if (!stopped && run.current) timer = setTimeout(tick, 1000 / rate.current);
        }
        timer = setTimeout(tick, 1000 / rate.current);
        return () => { stopped = true; clearTimeout(timer); };
    }, [playing]);
    const population = cells.split('1').length - 1;
    return <main className="observatory">
        <header><a className="identity" href="#"><span className="mark">✳</span> FIELD<span className="edition">/ 01</span></a>
            <span className="eyebrow">A small universe, computed in C++23</span><span className="engine"><i/> Native engine</span></header>
        <section className="scene">
            <div className="intro"><span className="eyebrow">CONWAY’S GAME OF LIFE</span><h1>Order.<br/>From almost nothing.</h1>
                <p>Three simple rules.<br/>An entire world of possibilities.</p></div>
            <div className="world"><div className="halo"/><Board cells={cells} flat={flat} toggle={index => {
                const next = state.current.cells.split(''); next[index] = next[index] === '1' ? '0' : '1'; replace(next.join(''), 'Custom');
            }}/></div>
            <aside className="glass telemetry" aria-label="Simulation statistics"><span className="eyebrow">OBSERVATION</span>
                <div><output data-testid="generation">{generation.toString().padStart(4, '0')}</output><span>Generation</span></div>
                <div><output data-testid="population">{population}</output><span>Living cells</span></div>
                <span className="boundary">32 × 32 · wrapping edges</span></aside>
            <div className="coordinates">B3 / S23 <span>CLICK THE FIELD TO PLANT A CELL</span> 32²</div>
        </section>
        <footer className="glass controls"><div className="transport">
            <button className="primary" data-testid="play" aria-pressed={playing} onClick={() => {run.current = !playing; setPlaying(!playing);}}>{playing ? 'Ⅱ Pause' : '▶ Play'}</button>
            <button data-testid="step" disabled={playing || busy} onClick={() => {void step();}}>Step ↗</button></div>
            <label className="speed">Tempo <input aria-label="Generations per second" type="range" min="1" max="24" value={speed} onChange={e => setSpeed(Number(e.target.value))}/><span>{speed}/s</span></label>
            <div className="patterns" aria-label="Starting pattern">{Object.keys(shapes).map(name => <button key={name} data-testid={'pattern-' + name.toLowerCase()} aria-pressed={pattern === name} onClick={() => replace(seed(name), name)}>{name}</button>)}
                <button data-testid="clear" onClick={() => replace(empty(), 'Empty')}>Clear</button></div>
            <button className="view" aria-pressed={flat} onClick={() => setFlat(!flat)}>{flat ? '◇ Spatial' : '▦ Plan view'}</button>
        </footer>
        <p className="caption">A bitset in C++. A world in your browser. <span>WebFront · React · no Node.js runtime</span></p>
        <p className="error" role="alert">{error}</p>
    </main>;
}
createRoot(document.getElementById('root')!).render(<Life/>);
