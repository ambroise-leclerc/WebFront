// The application's C++ commands (webfront::Commands): discovered through the bridge and run in this browser's session.
// Their output streams in through fire-and-forget calls; Ctrl+C cancels them in C++ without waiting for them to end.

const running = new Map();  // command number -> {write, exit}
let lastId = 0;

// The browser functions that C++ calls with the output and the exit of a command. A command this page no longer
// waits for (interrupted, or of an earlier page) is ignored.
function installReceivers() {
    const namespace = globalThis.webfront ??= {};
    namespace.commands ??= {
        output: (id, text) => running.get(id)?.write(text),
        exit: (id, code, error) => running.get(id)?.exit(code, error),
    };
}

/** The application's commands, as a Map from name to help text; empty for an application without webfront::Commands. */
export async function discoverCommands(bridge) {
    installReceivers();
    let listed;
    try { listed = JSON.parse(await bridge.cppFunction('webfront.commands.list')()); }
    catch (error) {
        if (/C\+\+ function 'webfront\.commands\.list' was not found/.test(error?.message)) return new Map();
        throw error;
    }
    return new Map(listed.map(({name, help}) => [name, help]));
}

const failure = (message, exitCode) => Object.assign(new Error(message), {exitCode});

/**
 * Runs a C++ command and resolves once it returns. It rejects with the command's exception (exit code 1), with its
 * non-zero exit code, or at once with exit code 130 when `signal` aborts: the command is then asked to stop, and its
 * late output and exit are ignored. `write` receives the output as it comes, with "\n" line endings.
 */
export function runCommand(bridge, name, args, {signal, write = () => {}} = {}) {
    installReceivers();
    lastId = lastId % 0xffffffff + 1;
    const id = lastId;
    return new Promise((resolve, reject) => {
        const finish = () => {
            running.delete(id);
            signal?.removeEventListener('abort', interrupt);
        };
        const interrupt = () => {
            finish();
            bridge.cppFunction('webfront.commands.cancel')(id).catch(() => {});
            reject(failure('Interrupted', 130));
        };
        if (signal?.aborted) return reject(failure('Interrupted', 130));
        running.set(id, {write, exit: (code, error) => {
            finish();
            if (code === 0) resolve('');
            else reject(failure(error || `Command ${name} exited with code ${code}`, code));
        }});
        signal?.addEventListener('abort', interrupt, {once: true});
        // A bridge that throws instead of rejecting must not leave the command registered.
        Promise.resolve().then(() => bridge.cppFunction('webfront.commands.start')(id, name, JSON.stringify(args))).catch(error => {
            if (!running.has(id)) return;
            finish();
            reject(error);
        });
    });
}
