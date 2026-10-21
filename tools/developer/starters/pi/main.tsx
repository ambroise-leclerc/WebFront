import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import './style.css';

declare const webFront: {
    ready: Promise<void>;
    cppFunction(name: string): (digits: number) => Promise<string>;
};

function Pi() {
    const [digits, setDigits] = useState(50);
    const [value, setValue] = useState('');
    const [error, setError] = useState('');
    async function calculate() {
        try {
            await webFront.ready;
            setValue(await webFront.cppFunction('pi')(digits));
            setError('');
        } catch (error) { setError(String(error)); }
    }
    return <main><h1>PI, calculated in C++</h1>
        <label>Decimals <input type="number" min="0" max="1000" value={digits}
            onChange={event => setDigits(Number(event.target.value))}/></label>
        <button onClick={calculate}>Calculate</button>
        <output>{value}</output><p role="alert">{error}</p>
    </main>;
}

createRoot(document.getElementById('root')!).render(<Pi/>);
