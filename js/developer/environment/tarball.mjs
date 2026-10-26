// npm tarballs in the browser: package specs, SHA-512 integrity, gzip with a size bound, and tar entries (ustar, pax
// and GNU long names).

const blockSize = 512;
// Largest unpacked tarball accepted, against decompression bombs.
export const maxUnpackedBytes = 256 * 1024 * 1024;
const utf8 = new TextDecoder();

// Splits "name[@version]", where the name may be scoped ("@scope/name@1.0.0"). The version is ""
// when absent.
export function parseSpec(spec) {
    const at = spec.indexOf("@", spec.startsWith("@") ? 1 : 0);
    return at < 0 ? {name: spec, version: ""} : {name: spec.slice(0, at), version: spec.slice(at + 1)};
}

// Checks bytes against an SRI integrity string ("sha512-<base64>", possibly several hashes
// separated by spaces) and returns the matching "sha512-…" hash. Only SHA-512 is accepted.
export async function verifyIntegrity(bytes, integrity) {
    const expected = String(integrity ?? "")
        .split(/\s+/)
        .filter((hash) => hash.startsWith("sha512-"))
        .map((hash) => hash.split("?")[0]);
    if (expected.length === 0)
        throw new Error("The package has no SHA-512 integrity");
    if (!globalThis.crypto?.subtle)
        throw new Error("Integrity checks need a secure context (https or localhost)");
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-512", bytes));
    const actual = "sha512-" + btoa(Array.from(digest, (byte) => String.fromCharCode(byte)).join(""));
    if (!expected.includes(actual))
        throw new Error(`Integrity check failed: expected ${expected[0]}, got ${actual}`);
    return actual;
}

// Gunzips bytes, failing once more than `limit` bytes come out.
export async function gunzip(bytes, limit = maxUnpackedBytes) {
    const reader = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip")).getReader();
    const chunks = [];
    let total = 0;
    for (let read = await reader.read(); !read.done; read = await reader.read()) {
        total += read.value.length;
        if (total > limit) {
            await reader.cancel();
            throw new Error(`The tarball unpacks to more than ${limit} bytes`);
        }
        chunks.push(read.value);
    }
    return new Uint8Array(await new Blob(chunks).arrayBuffer());
}

// A NUL-terminated header field.
function text(block, offset, length) {
    const field = block.subarray(offset, offset + length);
    const end = field.indexOf(0);
    return utf8.decode(end < 0 ? field : field.subarray(0, end));
}

// An octal number, or a base-256 one when the first byte has its high bit set (GNU and star).
function number(block, offset, length) {
    if (block[offset] & 0x80)
        return block.subarray(offset + 1, offset + length).reduce((value, byte) => value * 256 + byte, block[offset] & 0x7f);
    const digits = text(block, offset, length).trim();
    if (!/^[0-7]*$/.test(digits))
        throw new Error("Corrupt tar header");
    return digits === "" ? 0 : parseInt(digits, 8);
}

function verifyChecksum(header) {
    let sum = 0;
    for (let index = 0; index < blockSize; ++index)
        sum += index >= 148 && index < 156 ? 0x20 : header[index];
    if (number(header, 148, 8) !== sum)
        throw new Error("Corrupt tar header checksum");
}

// "<length> <key>=<value>\n" records of a pax extended header; lengths count bytes.
function paxRecords(data) {
    const records = {};
    let offset = 0;
    while (offset < data.length) {
        const space = data.indexOf(0x20, offset);
        const length = space < 0 ? NaN : Number(utf8.decode(data.subarray(offset, space)));
        if (!(length > 0) || offset + length > data.length)
            throw new Error("Corrupt pax header");
        const record = utf8.decode(data.subarray(space + 1, offset + length - 1));
        const equals = record.indexOf("=");
        records[record.slice(0, equals)] = record.slice(equals + 1);
        offset += length;
    }
    return records;
}

// The entry whose header starts at `offset`: its header, type flag, data and the offset of the next
// entry.
function readEntry(archive, offset) {
    const header = archive.subarray(offset, offset + blockSize);
    verifyChecksum(header);
    const size = number(header, 124, 12);
    const start = offset + blockSize;
    if (start + size > archive.length)
        throw new Error("Truncated tar archive");
    return {
        header,
        type: header[156] === 0 ? "0" : String.fromCharCode(header[156]),
        data: archive.subarray(start, start + size),
        next: start + Math.ceil(size / blockSize) * blockSize,
    };
}

// An archive ends at two zero blocks; one is enough to stop.
function isEnd(archive, offset) {
    return offset + blockSize > archive.length || archive.subarray(offset, offset + blockSize).every((byte) => byte === 0);
}

// The name field of a ustar header, after its prefix field.
function headerPath(header) {
    const name = text(header, 0, 100);
    const prefix = text(header, 257, 5) === "ustar" ? text(header, 345, 155) : "";
    return prefix ? `${prefix}/${name}` : name;
}

// Entries naming the next one: a pax extended header and a GNU long name. A pax global header names
// nothing WebFront uses.
const nameEntries = {
    x: (names, data) => ({...names, pax: paxRecords(data).path}),
    L: (names, data) => ({...names, long: text(data, 0, data.length)}),
    g: (names) => names,
};

// Reads a tar archive (ustar, with pax and GNU long names) and returns its regular files as
// {path, bytes}, in archive order. Directories, links and other entries are skipped.
export function readTar(archive) {
    const files = [];
    let names = {};
    for (let offset = 0; !isEnd(archive, offset);) {
        const {header, type, data, next} = readEntry(archive, offset);
        offset = next;
        if (Object.hasOwn(nameEntries, type)) {
            names = nameEntries[type](names, data);
            continue;
        }
        if (type === "0" || type === "7")
            files.push({path: names.pax ?? names.long ?? headerPath(header), bytes: data});
        names = {};
    }
    return files;
}
