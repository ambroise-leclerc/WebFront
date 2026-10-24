import React, {useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import './style.css';

declare const webFront: {ready: Promise<void>; cppFunction(name: string): (grid: Uint8Array) => Promise<Uint8Array>};
const size = 32;
const byteCount = size * size / 8;
const empty = () => new Uint8Array(byteCount);
const isAlive = (cells: Uint8Array, i: number) => (cells[i >> 3] & (1 << (i & 7))) !== 0;
function toggleCell(cells: Uint8Array, i: number) {
    const next = cells.slice();
    next[i >> 3] ^= 1 << (i & 7);
    return next;
}
const shapes: Record<string, number[][]> = {
    Glider: [[1, 0], [2, 1], [0, 2], [1, 2], [2, 2]],
    Blinker: [[0, 1], [1, 1], [2, 1]],
    Galaxy: [[1, 0], [2, 0], [0, 1], [1, 1], [1, 2]],
};
function seed(name: string) {
    const cells = empty();
    const offsets = name === 'Galaxy' ? [[7, 7], [21, 9], [13, 22]] : [[14, 14]];
    for (const [ox, oy] of offsets)
        for (const [x, y] of shapes[name]) {
            const i = (y + oy) * size + x + ox;
            cells[i >> 3] |= 1 << (i & 7);
        }
    return cells;
}

type Painter = CanvasRenderingContext2D;
type Cell = {x: number; y: number; alive: boolean};
type Point = {x: number; y: number};
function fillTile(context: Painter, points: number[][], fill: string, stroke?: string) {
    context.beginPath(); points.forEach(([x, y], i) => i ? context.lineTo(x, y) : context.moveTo(x, y));
    context.closePath(); context.fillStyle = fill; context.fill();
    if (stroke) { context.strokeStyle = stroke; context.lineWidth = .7; context.stroke(); }
}
function paintFlat(context: Painter, {x, y, alive}: Cell) {
    context.fillStyle = alive ? '#bcf8df' : 'rgba(159,202,194,.07)';
    context.fillRect(209 + x * 17, 65 + y * 17, 15, 15);
}
function paintSpatial(context: Painter, {x, y, alive}: Cell) {
    const px = 480 + (x - y) * 13, py = 115 + (x + y) * 7;
    const lift = alive ? 20 : 0;
    fillTile(context, [[px, py - lift], [px + 12, py + 6 - lift], [px, py + 12 - lift], [px - 12, py + 6 - lift]],
        alive ? '#c8ffe4' : 'rgba(145,190,182,.075)', alive ? '#eefff8' : 'rgba(160,207,195,.12)');
    if (!alive) return;
    fillTile(context, [[px - 12, py + 6 - lift], [px, py + 12 - lift], [px, py + 12], [px - 12, py + 6]], '#529a83');
    fillTile(context, [[px, py + 12 - lift], [px + 12, py + 6 - lift], [px + 12, py + 6], [px, py + 12]], '#7fc5a6');
}
function paintBoard(element: HTMLCanvasElement, cells: Uint8Array, flat: boolean) {
    const ratio = window.devicePixelRatio || 1;
    const {width, height} = element.getBoundingClientRect();
    element.width = Math.round(width * ratio); element.height = Math.round(height * ratio);
    const context = element.getContext('2d')!;
    context.scale(ratio * width / 960, ratio * height / 680);
    const paint = flat ? paintFlat : paintSpatial;
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) paint(context, {x, y, alive: isAlive(cells, y * size + x)});
}
// The cell under a point of the 960 x 680 scene, or -1 outside the board.
function cellAt({x: px, y: py}: Point, flat: boolean) {
    const x = flat ? Math.floor((px - 209) / 17) : Math.floor(((px - 480) / 13 + (py - 115) / 7) / 2);
    const y = flat ? Math.floor((py - 65) / 17) : Math.floor(((py - 115) / 7 - (px - 480) / 13) / 2);
    const inside = (value: number) => value >= 0 && value < size;
    return inside(x) && inside(y) ? y * size + x : -1;
}

function Board({cells, flat, toggle}: {cells: Uint8Array; flat: boolean; toggle: (index: number) => void}) {
    const canvas = useRef<HTMLCanvasElement>(null);
    useEffect(() => {
        const element = canvas.current!;
        const draw = () => paintBoard(element, cells, flat);
        draw();
        const observer = new ResizeObserver(draw); observer.observe(element);
        return () => observer.disconnect();
    }, [cells, flat]);
    return <canvas ref={canvas} role="img" aria-label="32 by 32 Game of Life. Use the pattern buttons and Step to explore."
        onClick={event => {
            const rect = event.currentTarget.getBoundingClientRect();
            const index = cellAt({x: (event.clientX - rect.left) * 960 / rect.width, y: (event.clientY - rect.top) * 680 / rect.height}, flat);
            if (index >= 0) toggle(index);
        }}/>;
}

type Session = {cells: Uint8Array; revision: number; pending: boolean; mounted: boolean};
const isCurrent = (session: Session, revision: number) => session.mounted && revision === session.revision;
// The next generation from C++, or null when the session moved on while waiting.
async function nextGeneration(session: Session, revision: number) {
    await webFront.ready;
    if (!isCurrent(session, revision)) return null;
    const next = await webFront.cppFunction('life.step')(session.cells);
    if (!isCurrent(session, revision)) return null;
    if (!(next instanceof Uint8Array) || next.length !== byteCount) throw new Error('Invalid C++ generation');
    return next;
}
const countBits = (cells: Uint8Array) => cells.reduce((count, byte) => { while (byte) { count += byte & 1; byte >>>= 1; } return count; }, 0);

function Telemetry({generation, population}: {generation: number; population: number}) {
    return <aside className="glass telemetry" aria-label="Simulation statistics"><span className="eyebrow">SIMULATION</span>
                <div><output data-testid="generation">{generation.toString().padStart(4, '0')}</output><span>Generation</span></div>
                <div><output data-testid="population">{population}</output><span>Living cells</span></div>
                <span className="boundary">32 × 32 · wrapping edges</span></aside>;
}

type ControlProps = {playing: boolean; busy: boolean; speed: number; flat: boolean; pattern: string;
    onPlay: () => void; onStep: () => void; onSpeed: (speed: number) => void; onPattern: (name: string) => void;
    onClear: () => void; onView: () => void};
function Controls({playing, busy, speed, flat, pattern, onPlay, onStep, onSpeed, onPattern, onClear, onView}: ControlProps) {
    return <footer className="glass controls"><div className="transport">
        <button className="primary" data-testid="play" aria-pressed={playing} onClick={onPlay}>{playing ? 'Ⅱ Pause' : '▶ Play'}</button>
        <button data-testid="step" disabled={playing || busy} onClick={onStep}>Step ↗</button></div>
        <label className="speed">Tempo <input aria-label="Generations per second" type="range" min="1" max="24" value={speed} onChange={e => onSpeed(Number(e.target.value))}/><span>{speed}/s</span></label>
        <div className="patterns" aria-label="Starting pattern">{Object.keys(shapes).map(name => <button key={name} data-testid={'pattern-' + name.toLowerCase()} aria-pressed={pattern === name} onClick={() => onPattern(name)}>{name}</button>)}
            <button data-testid="clear" onClick={onClear}>Clear</button></div>
        <button className="view" data-testid="view" aria-pressed={flat} onClick={onView}>{flat ? '◇ Spatial' : '▦ Plan view'}</button>
    </footer>;
}

type Flag<T> = {current: T};
// While playing, steps at the chosen rate; a step that is still pending delays the next tick.
function usePlayback(playing: boolean, run: Flag<boolean>, rate: Flag<number>, step: () => Promise<void>) {
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
}

function Life() {
    const [cells, setCells] = useState<Uint8Array>(() => seed('Galaxy'));
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
    function replace(value: Uint8Array, name: string) {
        run.current = false; setPlaying(false); state.current.revision++;
        state.current.cells = value; setCells(value); setGeneration(0); setPattern(name); setError('');
    }
    async function step() {
        const current = state.current;
        if (current.pending || !current.mounted) return;
        current.pending = true; setBusy(true);
        const revision = current.revision;
        try {
            const next = await nextGeneration(current, revision);
            if (next) { current.cells = next; setCells(next); setGeneration(value => value + 1); setError(''); }
        } catch (error) {
            if (isCurrent(current, revision)) { setError(String(error)); run.current = false; setPlaying(false); }
        } finally { current.pending = false; if (current.mounted) setBusy(false); }
    }
    usePlayback(playing, run, rate, step);
    const population = countBits(cells);
    return <main className="observatory">
        <header><a className="identity" href="#"><span className="mark">✳</span> WEBFRONT<span className="edition">/ LIFE</span></a>
            <span className="eyebrow">C++23 simulation · React interface</span><span className="engine"><i/> Native engine</span></header>
        <section className="scene">
            <div className="intro"><span className="eyebrow">CONWAY’S GAME OF LIFE</span><h1>Game of Life</h1>
                <p>32 × 32 grid · wrapping edges.<br/>B3/S23 rules computed in C++.</p></div>
            <div className="world"><div className="halo"/><Board cells={cells} flat={flat} toggle={index => replace(toggleCell(state.current.cells, index), 'Custom')}/></div>
            <Telemetry generation={generation} population={population}/>
            <div className="coordinates">B3 / S23 <span>CLICK A CELL TO TOGGLE IT</span> 32²</div>
        </section>
        <Controls playing={playing} busy={busy} speed={speed} flat={flat} pattern={pattern}
            onPlay={() => {run.current = !playing; setPlaying(!playing);}} onStep={() => {void step();}} onSpeed={setSpeed}
            onPattern={name => replace(seed(name), name)} onClear={() => replace(empty(), 'Empty')} onView={() => setFlat(!flat)}/>
        <p className="caption">C++: simulation · React: rendering <span>WebFront · React · no Node.js runtime</span></p>
        <p className="error" role="alert">{error}</p>
    </main>;
}
createRoot(document.getElementById('root')!).render(<Life/>);
