// Shared by the wf suite (module import) and consumer Jasmine (classic script). Keep a single budget per spec,
// below its Jasmine deadline, so even a sequence of stalled operations reports its current stage first (#504).
globalThis.webFrontTestAsync = {
    timeoutMs: 15000,
    specTimeoutMs: 20000,
    steps(timeoutMs = 15000, details = () => '') {
        const deadline = performance.now() + timeoutMs;
        const step = async (name, action) => {
            let timer;
            try {
                const remaining = deadline - performance.now();
                if (remaining <= 0) throw new Error(`timed out after ${timeoutMs}ms`);
                return await Promise.race([
                    Promise.resolve().then(action),
                    new Promise((_, reject) => {
                        timer = setTimeout(() => reject(new Error(`timed out after ${timeoutMs}ms`)), remaining);
                    }),
                ]);
            } catch (error) {
                const state = details();
                throw new Error(`${name}: ${error?.message ?? error}${state ? `; ${state}` : ''}`);
            } finally {
                clearTimeout(timer);
            }
        };
        return {
            step,
            async event(target, name, action, stage = name, accept = () => true) {
                let received, failed;
                const event = new Promise((resolve, reject) => {
                    received = event => { if (accept()) resolve(event); };
                    failed = () => reject(new Error(`error event (${target.error?.code}: ${target.error?.message})`));
                    target.addEventListener(name, received);
                    target.addEventListener('error', failed);
                });
                try {
                    // Register before triggering. Observe the event and play() together: either can stall or reject.
                    await step(stage, () => Promise.all([event, Promise.resolve().then(action)]));
                } finally {
                    target.removeEventListener(name, received);
                    target.removeEventListener('error', failed);
                }
            },
        };
    },
};
