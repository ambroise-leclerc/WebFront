// Reproducible ustar for the published npm environment and local registry fixtures.
export function tar(files) {
    const encoder = new TextEncoder();
    const chunks = [];
    const field = (header, offset, length, value) => header.set(encoder.encode(value).subarray(0, length), offset);
    for (const [name, bytes] of [...files].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)) {
        if (encoder.encode(name).length > 100) throw new Error('ustar fixture filename too long');
        const header = new Uint8Array(512);
        field(header, 0, 100, name);
        field(header, 100, 8, '0000644\0');
        field(header, 108, 8, '0000000\0');
        field(header, 116, 8, '0000000\0');
        field(header, 124, 12, bytes.length.toString(8).padStart(11, '0') + '\0');
        field(header, 136, 12, '00000000000\0');
        header.fill(32, 148, 156);
        header[156] = 48;
        field(header, 257, 6, 'ustar\0');
        field(header, 263, 2, '00');
        const checksum = header.reduce((sum, byte) => sum + byte, 0);
        field(header, 148, 8, checksum.toString(8).padStart(6, '0') + '\0 ');
        chunks.push(header, bytes, new Uint8Array((512 - bytes.length % 512) % 512));
    }
    chunks.push(new Uint8Array(1024));
    const output = new Uint8Array(chunks.reduce((sum, bytes) => sum + bytes.length, 0));
    let offset = 0;
    for (const bytes of chunks) { output.set(bytes, offset); offset += bytes.length; }
    return output;
}

// Deterministic gzip using stored blocks, usable before a registry has supplied a compressor.
const crcTable = Uint32Array.from({length: 256}, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
});
export function storedGzip(bytes) {
    const blocks = Math.max(1, Math.ceil(bytes.length / 0xffff));
    const out = new Uint8Array(10 + bytes.length + 5 * blocks + 8);
    const view = new DataView(out.buffer);
    out.set([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
    let at = 10;
    for (let block = 0; block < blocks; block++) {
        const chunk = bytes.subarray(block * 0xffff, (block + 1) * 0xffff);
        out[at++] = block === blocks - 1 ? 1 : 0;
        view.setUint16(at, chunk.length, true);
        view.setUint16(at + 2, ~chunk.length & 0xffff, true);
        out.set(chunk, at + 4);
        at += 4 + chunk.length;
    }
    let crc = 0xffffffff;
    for (const byte of bytes) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
    view.setUint32(at, (crc ^ 0xffffffff) >>> 0, true);
    view.setUint32(at + 4, bytes.length >>> 0, true);
    return out;
}
