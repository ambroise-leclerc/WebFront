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
