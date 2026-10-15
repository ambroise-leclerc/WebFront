// Browser part of the 'browser' terminal command that webtest/JasmineTest.cpp registers, driven by
// webtest/js/terminal-specs.mjs. The specs import this same module to observe a running command.
export const hooks = {onWait: null};

export async function run(args, io) {
    const [mode, ...rest] = args;
    if (mode === 'echo')
        return io.write(`args: ${JSON.stringify(rest)}, bridge: ${io.bridge === globalThis.webFront}\n`);
    if (mode === 'partial')
        return io.write('no line end');
    if (mode === 'fail') {
        io.write('started');
        throw new Error('browser command failed');
    }
    if (mode === 'wait') {
        io.write('waiting\n');
        await new Promise((resolve, reject) => {
            io.signal.addEventListener('abort', () => reject(io.signal.reason));
            hooks.onWait?.();
        });
    }
}
