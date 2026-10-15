/// @brief Browser side of the wf tool manager (#234), a terminal extension added by
/// webfront::ToolManager (include/components/ToolManager.hpp). 'wf search', 'wf install' and
/// 'wf open' run here, because the download speaks HTTPS; 'wf list', 'wf remove' and 'wf help' run
/// in C++. An installation fetches the packument and the tarball from the npm registry, checks the
/// tarball's SHA-512 integrity, unpacks it and sends its files through webfront.pkg.begin,
/// .writeFile and .commit.

const blockSize = 512;
/// Files cross the bridge in chunks of this many bytes.
export const chunkSize = 16384;
const utf8 = new TextDecoder();

/// Splits "name[@version]", where the name may be scoped ("@scope/name@1.0.0"). The version is ""
/// when absent.
export function parseSpec(spec) {
    const at = spec.indexOf("@", spec.startsWith("@") ? 1 : 0);
    return at < 0 ? {name: spec, version: ""} : {name: spec.slice(0, at), version: spec.slice(at + 1)};
}

function registryBase(registry) {
    const base = new URL(registry, globalThis.location?.href);
    if (!base.pathname.endsWith("/"))
        base.pathname += "/";
    return base;
}

// The registry encodes the slash of a scoped name.
const packumentUrl = (base, name) => new URL(name.replace("/", "%2f"), base);

async function fetchOk(url, fetchImpl) {
    const response = await fetchImpl(url);
    if (!response.ok)
        throw new Error(`${url}: HTTP ${response.status}`);
    return response;
}

async function fetchJson(url, fetchImpl) {
    return (await fetchOk(url, fetchImpl)).json();
}

function resolveManifest(packument, name, requested) {
    const tag = requested || "latest";
    const version = packument["dist-tags"]?.[tag] ?? requested;
    const manifest = packument.versions?.[version];
    if (!manifest)
        throw new Error(`${name} has no version '${tag}'`);
    return manifest;
}

/// Checks bytes against an SRI integrity string ("sha512-<base64>", possibly several hashes
/// separated by spaces) and returns the matching "sha512-…" hash. Only SHA-512 is accepted.
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

export async function gunzip(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
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

/// Reads a tar archive (ustar, with pax and GNU long names) and returns its regular files as
/// {path, bytes}, in archive order. Directories, links and other entries are skipped.
export function readTar(archive) {
    const files = [];
    let offset = 0;
    let pax = {};
    let longName = null;
    while (offset + blockSize <= archive.length) {
        const header = archive.subarray(offset, offset + blockSize);
        if (header.every((byte) => byte === 0))
            break;
        verifyChecksum(header);
        const size = number(header, 124, 12);
        const type = header[156] === 0 ? "0" : String.fromCharCode(header[156]);
        const start = offset + blockSize;
        if (start + size > archive.length)
            throw new Error("Truncated tar archive");
        const data = archive.subarray(start, start + size);
        offset = start + Math.ceil(size / blockSize) * blockSize;
        if (type === "x") {
            pax = paxRecords(data);
            continue;
        }
        if (type === "L") {
            longName = text(data, 0, data.length);
            continue;
        }
        if (type === "g")
            continue;
        const name = text(header, 0, 100);
        const prefix = text(header, 257, 5) === "ustar" ? text(header, 345, 155) : "";
        const path = pax.path ?? longName ?? (prefix ? `${prefix}/${name}` : name);
        pax = {};
        longName = null;
        if (type === "0" || type === "7")
            files.push({path, bytes: data});
    }
    return files;
}

/// The files of an npm tarball by path relative to the package root: the first path component
/// (normally "package/") is removed, as npm does. A later entry replaces an earlier one.
function packageFiles(entries) {
    const files = new Map();
    for (const {path, bytes} of entries) {
        const relative = path.replace(/^(\.?\/)+/, "").split("/").slice(1).join("/");
        if (relative !== "")
            files.set(relative, bytes);
    }
    return files;
}

function toolOf(files, name, version) {
    const manifest = files.get("package.json");
    if (!manifest)
        throw new Error(`${name}@${version} has no package.json`);
    const pkg = JSON.parse(utf8.decode(manifest));
    if (pkg.name !== name || pkg.version !== version)
        throw new Error(`The archive contains ${pkg.name}@${pkg.version}, not ${name}@${version}`);
    const tool = pkg.webfront;
    if (tool?.kind !== "tool" || typeof tool.entry !== "string")
        throw new Error(`${name} is not a WebFront tool: its package.json has no webfront field with kind "tool" and an entry`);
    return {entry: tool.entry.replace(/^(\.\/)+/, ""), title: typeof tool.title === "string" ? tool.title : name};
}

const pkgFunction = (bridge, name) => bridge.cppFunction(`webfront.pkg.${name}`);

/// Options shared by search() and install():
///   bridge    the WebFront bridge (globalThis.webFront)
///   registry  the npm registry URL (default: webfront.pkg.registry(), set in ToolManagerOptions)
///   fetch     the fetch function
async function settings({bridge = globalThis.webFront, registry, fetch: fetchImpl = (...args) => globalThis.fetch(...args)} = {}) {
    return {bridge, fetchImpl, base: registryBase(registry ?? (await pkgFunction(bridge, "registry")()))};
}

/// Searches the registry for packages with the webfront-tool keyword. Returns {name, version,
/// description} objects.
export async function search(term, options) {
    const {fetchImpl, base} = await settings(options);
    const url = new URL("-/v1/search", base);
    url.searchParams.set("text", `keywords:webfront-tool ${term}`.trim());
    url.searchParams.set("size", "20");
    const result = await fetchJson(url, fetchImpl);
    return (result.objects ?? []).map(({package: found}) => ({
        name: found.name,
        version: found.version,
        description: found.description ?? "",
    }));
}

/// Downloads, verifies and stores "name[@version or dist-tag]" (latest by default). `progress(text)`
/// reports the steps. Resolves to {name, version, title, entry, files}; rejects without changing the
/// installed version when anything fails.
export async function install(spec, {progress = () => {}, ...options} = {}) {
    const {name, version: requested} = parseSpec(spec);
    const {bridge, fetchImpl, base} = await settings(options);
    const packumentLocation = packumentUrl(base, name);
    const manifest = resolveManifest(await fetchJson(packumentLocation, fetchImpl), name, requested);
    const {version} = manifest;
    progress(`Downloading ${name}@${version}…`);
    const tarballUrl = new URL(manifest.dist?.tarball ?? "", packumentLocation);
    const tarball = new Uint8Array(await (await fetchOk(tarballUrl, fetchImpl)).arrayBuffer());
    const integrity = await verifyIntegrity(tarball, manifest.dist?.integrity);
    const files = packageFiles(readTar(await gunzip(tarball)));
    const {entry, title} = toolOf(files, name, version);
    if (!files.has(entry))
        throw new Error(`The entry page ${entry} of ${name} is not in the package`);

    progress(`Installing ${files.size} files…`);
    await pkgFunction(bridge, "begin")(name, version);
    const writeFile = pkgFunction(bridge, "writeFile");
    for (const [path, bytes] of files) {
        let offset = 0;
        do {
            const chunk = bytes.subarray(offset, offset + chunkSize);
            await writeFile(name, version, path, offset, chunk);
            offset += chunk.length;
        } while (offset < bytes.length);
    }
    await pkgFunction(bridge, "commit")(name, version, integrity, entry, title);
    return {name, version, title, entry, files: files.size};
}

/// The installed tools, from the pages of webfront.pkg.list(): {name, version, url, title} objects.
export async function installed(bridge = globalThis.webFront) {
    const list = pkgFunction(bridge, "list");
    const tools = [];
    for (;;) {
        const lines = (await list(tools.length)).split("\n").filter((line) => line !== "");
        if (lines.length === 0)
            return tools;
        for (const line of lines) {
            const [name, version, url, title] = line.split("\t");
            tools.push({name, version, url, title});
        }
    }
}

/// Opens the entry page of an installed tool in a new tab or window, and resolves to its URL.
export async function open(name, bridge = globalThis.webFront) {
    const tool = (await installed(bridge)).find((candidate) => candidate.name === name);
    if (!tool)
        throw new Error(`${name} is not installed`);
    return {...tool, opened: globalThis.open(tool.url, "_blank") !== null};
}

function requireOne(args) {
    if (args.length !== 1)
        throw new Error("Wrong number of arguments. Type 'wf help'.");
    return args[0];
}

async function wf([command, ...args], {bridge, write}) {
    switch (command) {
    case "search": {
        const found = await search(args.join(" "), {bridge});
        if (found.length === 0)
            return "No tool found.";
        const width = Math.max(...found.map(({name, version}) => name.length + version.length + 1));
        return found.map(({name, version, description}) => `${name}@${version}`.padEnd(width + 2) + description).join("\n");
    }
    case "install": {
        const tool = await install(requireOne(args), {bridge, progress: (step) => write(step + "\n")});
        return `Installed ${tool.name}@${tool.version}: ${tool.title}. Type 'wf open ${tool.name}'.`;
    }
    case "open": {
        const tool = await open(requireOne(args), bridge);
        return tool.opened ? `Opened ${tool.title}: ${tool.url}` : `The browser blocked the new window. Open ${tool.url}`;
    }
    default:
        return undefined;  // 'wf list', 'wf remove' and 'wf help' run in C++
    }
}

/// Terminal browser commands (see Terminal::extension).
export const commands = {wf};
