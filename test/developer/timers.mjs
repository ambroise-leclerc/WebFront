// Advance selected deadlines while keeping a real-time fallback for every timer.
// Native timer IDs remain cancellable after the scope restores the browser globals.
export async function withTimers(delays, run) {
    const originalSet = globalThis.setTimeout;
    const originalClear = globalThis.clearTimeout;
    const originalNow = Date.now;
    const now = Date.now();
    Date.now = () => now;
    const pending = new Map();
    globalThis.setTimeout = (callback, delay, ...args) => {
        if (!delays.includes(delay)) return originalSet(callback, delay, ...args);
        const invoke = () => { pending.delete(id); callback(...args); };
        const id = originalSet(invoke, delay);
        pending.set(id, {delay, invoke});
        return id;
    };
    globalThis.clearTimeout = id => {
        pending.delete(id);
        originalClear(id);
    };
    try {
        return await run({pending: () => pending.size, fire(delay) {
            const entry = [...pending].find(([, timer]) => timer.delay === delay);
            if (!entry) throw new Error(`No pending ${delay} ms deadline`);
            originalClear(entry[0]);
            entry[1].invoke();
        }});
    } finally {
        Date.now = originalNow;
        globalThis.setTimeout = originalSet;
        globalThis.clearTimeout = originalClear;
    }
}

// A readiness handshake cannot arrive after its command has already ended.
export const untilReady = (ready, running) => Promise.race([ready, running.then(result => {
    throw new Error(`The command ended before readiness: ${result?.error?.message ?? result?.message ?? 'completed'}`);
})]);
