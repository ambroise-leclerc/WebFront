// Bridge and network transfers shared by the developer environment: project reads, staged publications, bounded downloads.
export const blockSize = 256 * 1024;
export const call = (bridge, name, ...args) => bridge.cppFunction(`webfront.developer.${name}`)(...args);

export async function readProject(bridge, path) {
    const chunks = [];
    let offset = 0;
    for (;;) {
        const chunk = await call(bridge, 'read', path, offset);
        chunks.push(chunk);
        offset += chunk.length;
        if (chunk.length < blockSize) break;
        if (offset > 256 * 1024 * 1024) throw new Error('Project file is too large');
    }
    return new Uint8Array(await new Blob(chunks).arrayBuffer());
}

async function writeChunks(bridge, id, path, bytes, signal) {
    for (let offset = 0; offset < bytes.length || offset === 0; offset += blockSize) {
        signal?.throwIfAborted();
        await call(bridge, 'write', id, path, offset, bytes.subarray(offset, offset + blockSize));
    }
}

export async function publish(bridge, destination, files, {directory = true, signal, replace = true} = {}) {
    const id = await call(bridge, replace ? 'begin' : 'beginNew', destination, directory);
    try {
        for (const [path, bytes] of files) await writeChunks(bridge, id, path, bytes, signal);
        signal?.throwIfAborted();
        await call(bridge, 'commit', id);
    } catch (error) {
        await call(bridge, 'abort', id).catch(() => {});
        throw error;
    }
}

async function readLimited(reader, limit) {
    const chunks = [];
    let size = 0;
    try {
        for (;;) {
            const {done, value} = await reader.read();
            if (done) return chunks;
            size += value.length;
            if (size > limit) throw new Error('Compressed download exceeds its byte limit');
            chunks.push(value);
        }
    } catch (error) {
        await reader.cancel().catch(() => {});
        throw error;
    } finally { reader.releaseLock(); }
}

export async function responseBytes(response, limit = 64 * 1024 * 1024) {
    if (!response.ok) throw new Error(`Download: HTTP ${response.status}`);
    if (Number(response.headers.get('content-length')) > limit) throw new Error('Compressed download exceeds its byte limit');
    return new Uint8Array(await new Blob(await readLimited(response.body.getReader(), limit)).arrayBuffer());
}
