// Optional xterm.js adapter. The application supplies its own Terminal and FitAddon instances.
export async function mount(element, {
    terminal, fit, webFront = globalThis.webFront, name = 'webfront.pty', restart = false,
    onExit = () => {}, onError = error => console.error(error), onTruncated = () => {},
}) {
    await webFront.ready;
    const generation = await webFront.cppFunction(name + '.open')(restart);
    const call = operation => (...args) => webFront.cppFunction(name + '.' + operation)(generation, ...args);
    const read = call('read');
    const write = call('write');
    const resize = call('resize');
    const hangUp = call('hangUp');

    const limit = 64 * 1024;
    const encoder = new TextEncoder();
    const input = [];
    let pending = 0;
    let writing = false;
    let sizing = false;
    let sentSize;
    let disposed = false;
    let cursor = 0;

    const sendInput = async () => {
        writing = true;
        try {
            while (!disposed && input.length) {
                const bytes = input[0];
                for (let offset = 0; offset < bytes.length && !disposed; offset += 4096) {
                    if (!await write(bytes.subarray(offset, offset + 4096)))
                        throw new Error('PTY input queue is full or closed');
                }
                if (!disposed) {
                    input.shift();
                    pending -= bytes.length;
                }
            }
        } catch (error) {
            input.length = 0;
            pending = 0;
            if (!disposed) onError(error);
        } finally {
            writing = false;
        }
    };

    const enqueue = (text, encode) => {
        if (disposed || !text.length) return;
        if (text.length <= limit - pending) {
            const bytes = encode(text);
            if (bytes.length <= limit - pending) {
                input.push(bytes);
                pending += bytes.length;
                if (!writing) void sendInput();
                return;
            }
        }
        onError(new Error('PTY input exceeds the browser queue capacity'));
    };

    const fitting = async () => {
        if (disposed || sizing) return;
        sizing = true;
        try {
            for (;;) {
                fit.fit();
                const {cols, rows} = terminal;
                if (sentSize?.cols === cols && sentSize?.rows === rows) break;
                if (!cols || !rows) break;
                await resize(cols, rows);
                if (disposed) break;
                sentSize = {cols, rows};
                // Recheck in case a ResizeObserver callback arrived while awaiting the bridge.
            }
        } catch (error) {
            if (!disposed) onError(error);
        } finally {
            sizing = false;
        }
    };

    const poll = async () => {
        try {
            const [next, base64, truncated, exited, code] = await read(cursor);
            if (disposed) return;
            cursor = next;
            if (truncated) {
                terminal.reset();
                onTruncated();
            }
            if (base64) {
                const bytes = Uint8Array.from(atob(base64), character => character.charCodeAt(0));
                // xterm decodes UTF-8 incrementally and acknowledges consumption before the next read.
                await new Promise(resolve => terminal.write(bytes, resolve));
            }
            if (disposed) return;
            if (exited) {
                dispose();
                onExit(code);
            } else {
                void poll();
            }
        } catch (error) {
            if (disposed) return;
            dispose();
            onError(error);
        }
    };

    const dispose = () => {
        if (disposed) return;
        disposed = true;
        observer.disconnect();
        data.dispose();
        binary.dispose();
        input.length = 0;
    };

    terminal.loadAddon(fit);
    terminal.open(element);
    const data = terminal.onData(text => enqueue(text, value => encoder.encode(value)));
    const binary = terminal.onBinary(text => enqueue(text, value => Uint8Array.from(value, ch => ch.charCodeAt(0))));
    const observer = new ResizeObserver(() => { void fitting(); });
    observer.observe(element);
    void fitting();
    void poll();
    return { dispose, hangUp: () => hangUp() };
}
