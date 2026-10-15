/// @brief Browser part of the wf tool manager (include/components/ToolManager.hpp): 'wf search',
/// 'wf install' and 'wf open', run by the terminal component as a browser command. The browser
/// downloads over HTTPS, checks the integrity hash, unpacks the tarball and sends the files to C++
/// through webfront.pkg.begin, .writeFile and .commit (see docs/UI_COMPONENTS.md).

/// Bytes per webfront.pkg.writeFile call.
export const chunkSize = 256 * 1024;
/// Largest unpacked tarball accepted, against decompression bombs.
export const maxUnpackedBytes = 256 * 1024 * 1024;

const hashAlgorithms = new Map([["sha512", "SHA-512"], ["sha384", "SHA-384"], ["sha256", "SHA-256"]]);

/// Entry point called by the terminal with the arguments ToolManager passes:
///   ["search", registry, term], ["install", registry, name, version or ""], ["open", url].
export async function run([command, ...args], io) {
    if (command === "search")
        return search(args, io);
    if (command === "install")
        return install(args, io);
    if (command === "open")
        return open(args, io);
    throw new Error(`Unknown wf browser command '${command}'`);
}

function registryBase(registry) {
    return new URL(registry.endsWith("/") ? registry : registry + "/", location.href);
}

async function fetchResponse(url, signal, what) {
    const response = await fetch(url, {signal});
    if (!response.ok)
        throw new Error(response.status === 404 ? `${what} not found (${url})` : `${what}: HTTP ${response.status} (${url})`);
    return response;
}

async function search([registry, term], io) {
    const url = new URL("-/v1/search", registryBase(registry));
    url.searchParams.set("text", `keywords:webfront-tool ${term}`.trim());
    url.searchParams.set("size", "20");
    const {objects = []} = await (await fetchResponse(url, io.signal, "Search")).json();
    if (objects.length === 0)
        return io.write(term ? `No tool found for '${term}'.` : "No tool found.");
    const labels = objects.map(({package: found}) => `${found.name}@${found.version}`);
    const width = Math.max(...labels.map((label) => label.length));
    io.write(objects.map(({package: found}, index) =>
        `${labels[index].padEnd(width + 2)}${oneLine(found.description ?? "")}`.trimEnd()).join("\n"));
}

function open([url], io) {
    const href = new URL(url, location.href).href;
    const opened = globalThis.open(href, "_blank");
    io.write(opened ? `Opened ${href}` : `The browser blocked the new window. Open ${href}`);
}

async function install([registry, name, requested], io) {
    const base = registryBase(registry);
    const packument = await (await fetchResponse(new URL(name.replace("/", "%2f"), base), io.signal, `Package ${name}`)).json();
    const version = packument["dist-tags"]?.[requested || "latest"] ?? requested;
    const manifest = packument.versions?.[version];
    if (!manifest)
        throw new Error(`Version '${requested || "latest"}' of ${name} not found`);
    requireTool(manifest, name, version);
    if (!manifest.dist?.tarball)
        throw new Error(`The registry gives no tarball for ${name}@${version}`);

    io.write(`Downloading ${name}@${version}…`);
    const tarball = new Uint8Array(await (await fetchResponse(new URL(manifest.dist?.tarball, base), io.signal, "Tarball")).arrayBuffer());
    const integrity = await verifyIntegrity(tarball, manifest.dist?.integrity);
    const files = packageFiles(readTar(await gunzip(tarball, maxUnpackedBytes)));

    const packageJson = parsePackageJson(files);
    if (packageJson.name !== name || packageJson.version !== version)
        throw new Error(`The tarball contains ${packageJson.name}@${packageJson.version}, not ${name}@${version}`);
    requireTool(packageJson, name, version);
    const entry = packageJson.webfront.entry ?? "index.html";
    if (!files.has(entry))
        throw new Error(`Entry page '${entry}' is missing from ${name}@${version}`);
    const title = oneLine(packageJson.webfront.title ?? packageJson.description ?? name).slice(0, 200);

    await upload({name, version, integrity, entry, title}, files, io);
    io.write(`\r\x1b[KInstalled ${name}@${version}: ${title}. Type 'wf open ${name}' to open it.`);
}

function requireTool(manifest, name, version) {
    if (manifest.webfront?.kind !== "tool")
        throw new Error(`${name}@${version} is not a WebFront tool: its package.json has no "webfront": {"kind": "tool"}`);
}

function parsePackageJson(files) {
    const bytes = files.get("package.json");
    if (!bytes)
        throw new Error("The tarball has no package.json");
    return JSON.parse(new TextDecoder().decode(bytes));
}

function oneLine(text) {
    return String(text).replace(/[\x00-\x1f\x7f]+/g, " ").trim();
}

async function upload(tool, files, io) {
    const call = (name) => io.bridge.cppFunction(`webfront.pkg.${name}`);
    const [begin, writeFile, commit] = [call("begin"), call("writeFile"), call("commit")];
    await begin(tool.name, tool.version);
    let count = 0;
    for (const [path, bytes] of files) {
        // An empty file still needs one call to be created.
        for (let offset = 0; offset === 0 || offset < bytes.length; offset += chunkSize) {
            io.signal.throwIfAborted();
            await writeFile(tool.name, tool.version, path, offset, bytes.subarray(offset, offset + chunkSize));
        }
        io.write(`\r\x1b[KInstalling ${tool.name}@${tool.version}: ${++count}/${files.size} files`);
    }
    io.signal.throwIfAborted();
    await commit(tool.name, tool.version, tool.integrity, tool.entry, tool.title);
}

/// Checks bytes against a Subresource Integrity string ("sha512-<base64>", possibly several
/// space-separated hashes) with the strongest algorithm it lists, and returns the matching hash.
export async function verifyIntegrity(bytes, integrity) {
    const hashes = String(integrity ?? "").split(/\s+/)
        .map((hash) => /^(sha512|sha384|sha256)-([A-Za-z0-9+/]+={0,2})$/.exec(hash))
        .filter(Boolean);
    if (hashes.length === 0)
        throw new Error("The registry gives no SHA-256, SHA-384 or SHA-512 integrity hash for the tarball");
    if (!globalThis.crypto?.subtle)
        throw new Error("Integrity checks need a secure context: open the application on localhost or over HTTPS");
    const algorithm = [...hashAlgorithms.keys()].find((name) => hashes.some(([, used]) => used === name));
    const digest = new Uint8Array(await crypto.subtle.digest(hashAlgorithms.get(algorithm), bytes));
    const encoded = btoa(String.fromCharCode(...digest));
    const match = hashes.find(([, used, value]) => used === algorithm && value === encoded);
    if (!match)
        throw new Error(`Integrity check failed: the tarball's ${algorithm} is ${algorithm}-${encoded}`);
    return match[0];
}

/// Gunzips bytes, failing once more than `limit` bytes come out.
export async function gunzip(bytes, limit) {
    const reader = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip")).getReader();
    const chunks = [];
    let total = 0;
    for (;;) {
        const {done, value} = await reader.read();
        if (done)
            break;
        total += value.length;
        if (total > limit) {
            await reader.cancel();
            throw new Error(`The tarball unpacks to more than ${limit} bytes`);
        }
        chunks.push(value);
    }
    const result = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
        result.set(chunk, offset);
        offset += chunk.length;
    }
    return result;
}

function field(header, offset, length) {
    const bytes = header.subarray(offset, offset + length);
    const end = bytes.indexOf(0);
    return new TextDecoder().decode(end < 0 ? bytes : bytes.subarray(0, end));
}

function octal(header, offset, length) {
    if (header[offset] & 0x80)
        throw new Error("Tar entries over 8 GiB are not supported");
    const text = field(header, offset, length).trim();
    if (!/^[0-7]*$/.test(text))
        throw new Error("Malformed tar header");
    return text === "" ? 0 : parseInt(text, 8);
}

// The checksum is the sum of the header's bytes, its own field counted as spaces.
function checkChecksum(header) {
    let sum = 0;
    for (let index = 0; index < 512; ++index)
        sum += index >= 148 && index < 156 ? 0x20 : header[index];
    if (sum !== octal(header, 148, 8))
        throw new Error("Corrupted tar header");
}

// Records of a pax extended header: "<length> <key>=<value>\n", the length counting the whole record.
function parsePax(data) {
    const records = {};
    let offset = 0;
    while (offset < data.length) {
        const space = data.indexOf(0x20, offset);
        const length = space < 0 ? NaN : parseInt(new TextDecoder().decode(data.subarray(offset, space)), 10);
        if (!(length > 0) || offset + length > data.length)
            throw new Error("Malformed pax header");
        const record = new TextDecoder().decode(data.subarray(space + 1, offset + length - 1));
        const equals = record.indexOf("=");
        if (equals > 0)
            records[record.slice(0, equals)] = record.slice(equals + 1);
        offset += length;
    }
    return records;
}

/// Reads the regular files of a tar archive (ustar, with pax and GNU long names) as
/// [{path, bytes}]. Directories, links and other entries are skipped.
export function readTar(archive) {
    const files = [];
    let extended = {};
    let longName = null;
    for (let offset = 0; offset + 512 <= archive.length;) {
        const header = archive.subarray(offset, offset + 512);
        if (header.every((byte) => byte === 0))
            break;
        checkChecksum(header);
        const size = octal(header, 124, 12);
        const type = header[156] === 0 ? "0" : String.fromCharCode(header[156]);
        const start = offset + 512;
        if (start + size > archive.length)
            throw new Error("Truncated tar archive");
        const data = archive.subarray(start, start + size);
        offset = start + Math.ceil(size / 512) * 512;
        if (type === "x") {
            extended = parsePax(data);
            continue;
        }
        if (type === "L") {
            longName = field(data, 0, data.length);
            continue;
        }
        if (type === "g")
            continue;
        const prefix = field(header, 257, 6) === "ustar" ? field(header, 345, 155) : "";
        const name = field(header, 0, 100);
        const path = extended.path ?? longName ?? (prefix ? `${prefix}/${name}` : name);
        extended = {};
        longName = null;
        if (type === "0" || type === "7")
            files.push({path, bytes: data});
    }
    return files;
}

/// The files of an npm tarball by path relative to the package root: the first path component
/// ("package/" in npm's tarballs) is removed, and a later entry replaces an earlier one.
export function packageFiles(entries) {
    const files = new Map();
    for (const {path, bytes} of entries) {
        const relative = path.replace(/^(\.\/)+/, "");
        const slash = relative.indexOf("/");
        if (slash > 0 && slash < relative.length - 1)
            files.set(relative.slice(slash + 1), bytes);
    }
    return files;
}
