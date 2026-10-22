// wf check inside its Worker: TypeScript compiles synchronously over an in-memory snapshot loaded beforehand.
// Phase 1 (asynchronous) reads the lib files of the locked typescript, the declaration files of the locked graph and
// the project files that tsconfig.json or the manifest entries cover, within bounds. Phase 2 creates the program with
// a CompilerHost over that snapshot: a file absent from it does not exist.
import {packageRoot} from './paths.mjs';

export const snapshotBounds = Object.freeze({files: 10000, bytes: 64 * 1024 * 1024, entries: 50000});
export const projectRoot = '/project';
const packagesRoot = '/packages';
export const configPath = `${projectRoot}/tsconfig.json`;
// Never listed: installed packages come from the lock, wf's own files are not sources.
const skippedDirectories = new Set(['node_modules', '.webfront', '.git']);
const maxConfigPasses = 16;

/** The options without a tsconfig.json; wf check always adds noEmit. */
export const defaultCompilerOptions = Object.freeze({
    target: 'ES2022', module: 'ESNext', moduleResolution: 'Bundler', jsx: 'react-jsx',
    lib: ['ES2022', 'DOM', 'DOM.Iterable'], strict: true, isolatedModules: true, skipLibCheck: true,
});
// Nothing is written: no emitted file, declaration or build information.
const noEmit = {noEmit: true, emitDeclarationOnly: false, composite: false, incremental: false};

const decoder = new TextDecoder();
const declaration = /\.d\.[cm]?ts$/;
const missingFile = error => /Cannot read project file:/.test(error?.message);

/** A module evaluating the CommonJS lib/typescript.js without require, so that TypeScript sees no Node.js system. */
export const typescriptModule = source =>
    `const module = {exports: {}};\n(function (module, exports, require) {\n${source}\n}).call(globalThis, module, module.exports, undefined);\nexport default module.exports;\n`;

const splitName = parts => parts[0]?.startsWith('@') ? [parts.slice(0, 2).join('/'), parts.slice(2)] : [parts[0], parts.slice(1)];

/**
 * The locked graph as TypeScript sees it, without flattening: the roots in /project/node_modules, and package <id> at
 * /packages/<id>/node_modules/<name>, where its dependencies are its siblings. realpath() maps every path to that
 * canonical place, so TypeScript resolves a package's imports from its own dependencies.
 */
export function createLayout(lock) {
    const packages = lock?.packages ?? {};
    const roots = new Map(Object.entries(lock?.roots ?? {}).map(([name, root]) => [name, root.id]));
    const visible = owner => owner === null ? roots
        : new Map([[packages[owner].name, owner], ...Object.entries(packages[owner].dependencies ?? {})]);
    const packagePath = (id, path = '') => [packagesRoot, id, 'node_modules', packages[id].name, path].filter(Boolean).join('/');

    // {kind: 'project', path} | {kind: 'package', id, path} | {kind: 'modules', names} | null
    function locate(path) {
        const parts = path.split('/').filter(Boolean);
        let owner = null;
        let rest;
        if (parts[0] === 'project') {
            rest = parts.slice(1);
            if (rest[0] !== 'node_modules') return {kind: 'project', path: rest.join('/')};
        } else if (parts[0] === 'packages' && Object.hasOwn(packages, parts[1]) && parts[2] === 'node_modules') {
            owner = parts[1];
            rest = parts.slice(2);
        } else return null;
        const names = visible(owner);
        const inside = rest.slice(1);
        if (!inside.length) return {kind: 'modules', names: [...new Set([...names.keys()].map(name => name.split('/')[0]))]};
        if (inside.length === 1 && inside[0].startsWith('@'))
            return {kind: 'modules', names: [...names.keys()].filter(name => name.startsWith(`${inside[0]}/`)).map(name => name.split('/')[1])};
        const [name, inner] = splitName(inside);
        return names.has(name) ? {kind: 'package', id: names.get(name), path: inner.join('/')} : null;
    }

    const canonical = location => location.kind === 'project'
        ? [projectRoot, location.path].filter(Boolean).join('/') : packagePath(location.id, location.path);
    return {locate, canonical, packagePath};
}

const parentOf = path => path.slice(0, path.lastIndexOf('/'));
const nameOf = path => path.slice(path.lastIndexOf('/') + 1);

/**
 * The files TypeScript may see, by canonical path, and the reads that missed. Adding a file beyond the bounds skips
 * it: the snapshot then records that it is truncated, and the files it skipped do not exist for the compiler.
 */
export function createSnapshot(lock, bounds = snapshotBounds) {
    const layout = createLayout(lock);
    const files = new Map();
    const directories = new Set([projectRoot]);
    const misses = new Set();
    let bytes = 0;
    let skipped = 0;
    const addDirectories = path => {
        for (let directory = parentOf(path); directory && !directories.has(directory); directory = parentOf(directory))
            directories.add(directory);
    };
    for (const id of Object.keys(lock?.packages ?? {})) {
        directories.add(layout.packagePath(id));
        addDirectories(layout.packagePath(id));
    }
    const resolve = path => {
        const location = layout.locate(path);
        return location && location.kind !== 'modules' ? layout.canonical(location) : null;
    };

    return {
        layout, files, misses,
        get bytes() { return bytes; },
        get skipped() { return skipped; },
        get truncated() { return skipped > 0; },
        /** Adds a file by its canonical path; false when the bounds leave no room for it. */
        add(path, data) {
            if (files.has(path)) return true;
            if (files.size >= bounds.files || bytes + data.length > bounds.bytes) {
                ++skipped;
                return false;
            }
            files.set(path, decoder.decode(data));
            bytes += data.length;
            addDirectories(path);
            return true;
        },
        /** Counts a file left out once the snapshot is truncated. */
        skip() {
            ++skipped;
            return false;
        },
        addDirectory(path) {
            directories.add(path);
            addDirectories(path);
        },
        realpath: path => resolve(path) ?? path,
        fileExists(path) {
            const real = resolve(path);
            if (real !== null && files.has(real)) return true;
            misses.add(path);
            return false;
        },
        readFile(path) {
            const real = resolve(path);
            if (real !== null && files.has(real)) return files.get(real);
            misses.add(path);
            return undefined;
        },
        directoryExists(path) {
            const location = layout.locate(path);
            if (location?.kind === 'modules') return location.names.length > 0;
            return location !== null && directories.has(layout.canonical(location));
        },
        // The subdirectories of a directory, by name.
        getDirectories(path) {
            const location = layout.locate(path);
            if (!location) return [];
            if (location.kind === 'modules') return location.names;
            const real = layout.canonical(location);
            const names = [...directories].filter(directory => parentOf(directory) === real).map(nameOf);
            if (real === projectRoot && Object.keys(lock?.roots ?? {}).length) names.push('node_modules');
            return names;
        },
        // Files and subdirectories of a project directory, for the tsconfig.json include patterns.
        entries(path) {
            const real = resolve(path) ?? path;
            const children = paths => [...paths].filter(item => parentOf(item) === real).map(nameOf);
            return {files: children(files.keys()), directories: children(directories)};
        },
    };
}

async function readOptional(read, path) {
    try { return await read(path); }
    catch (error) {
        if (missingFile(error)) return null;
        throw error;
    }
}

/**
 * Lists project directories through `list` (names, with a trailing slash for directories), recursively when asked.
 * Returns the project-relative paths of the files found; the directories go into the snapshot.
 */
async function listProject(snapshot, list, directories, bounds, signal) {
    const found = [];
    let entries = 0;
    const visit = async (directory, recursive) => {
        signal?.throwIfAborted();
        let names;
        try { names = await list(directory); }
        catch (error) {
            if (/Cannot list project directory/.test(error?.message)) return;
            throw error;
        }
        snapshot.addDirectory([projectRoot, directory].filter(Boolean).join('/'));
        for (const name of names) {
            if (++entries > bounds.entries)
                throw new Error(`wf check: the project directories hold more than ${bounds.entries} entries; narrow the include patterns of tsconfig.json`);
            const path = [directory, name.replace(/\/$/, '')].filter(Boolean).join('/');
            if (!name.endsWith('/')) found.push(path);
            else if (recursive && !skippedDirectories.has(name.slice(0, -1))) await visit(path, true);
        }
    };
    for (const [directory, recursive] of directories) await visit(directory, recursive);
    return found;
}

const projectRelative = path => path === projectRoot ? '' : path.startsWith(`${projectRoot}/`) ? path.slice(projectRoot.length + 1) : null;

function configHost(ts, snapshot) {
    return {
        useCaseSensitiveFileNames: true,
        readDirectory: (root, extensions, excludes, includes, depth) =>
            ts.matchFiles(root, extensions, excludes, includes, true, projectRoot, depth, snapshot.entries, snapshot.realpath),
        fileExists: snapshot.fileExists,
        readFile: snapshot.readFile,
        directoryExists: snapshot.directoryExists,
        getDirectories: snapshot.getDirectories,
        realpath: snapshot.realpath,
    };
}

// The configuration's own text is kept apart from the snapshot, which may have no room left for it.
function parseConfig(ts, snapshot, text) {
    const source = ts.parseJsonText(configPath, text);
    return ts.parseJsonSourceFileConfigFileContent(source, configHost(ts, snapshot), projectRoot, undefined, configPath);
}

/**
 * Loads into the snapshot the files that the previous reads missed and that exist: a project file read through the
 * bridge, or a file of a locked package. Returns whether one was added.
 */
async function loadMisses(snapshot, {read, lock}, attempted) {
    let added = false;
    for (const path of [...snapshot.misses]) {
        snapshot.misses.delete(path);
        const location = snapshot.layout.locate(path);
        if (!location || location.kind === 'modules') continue;
        const real = snapshot.layout.canonical(location);
        if (attempted.has(real)) continue;
        attempted.add(real);
        let bytes = null;
        if (location.kind === 'project' && location.path) bytes = await readOptional(read, location.path);
        else if (location.kind === 'package' && lock.packages[location.id].files.includes(location.path))
            bytes = await read(`${packageRoot}/${lock.packages[location.id].key}/${location.path}`);
        if (bytes !== null && snapshot.add(real, bytes)) added = true;
        if (snapshot.truncated) break;
    }
    return added;
}

// The extensions of the project files TypeScript may resolve under these options.
function projectExtensions(options) {
    const extensions = ['.ts', '.tsx', '.mts', '.cts'];
    if (options.allowJs) extensions.push('.js', '.jsx', '.mjs', '.cjs');
    if (options.resolveJsonModule) extensions.push('.json');
    return extensions;
}

// The project files of the snapshot: the sources TypeScript may resolve, and the package.json files that give the
// module format of the files under them.
const projectSource = options => path =>
    nameOf(`/${path}`) === 'package.json' || projectExtensions(options).some(extension => path.endsWith(extension));

/**
 * Phase 1. `read(path)` and `list(directory)` take project-relative paths and return Promises; `typescript` is the id
 * of the locked typescript package. Returns the snapshot and what phase 2 compiles: the root files, the options and
 * the configuration diagnostics.
 */
export async function loadSnapshot(ts, {read, list, lock, typescript, entries = [], bounds = snapshotBounds, signal}) {
    const snapshot = createSnapshot(lock, bounds);
    const loadPackageFile = async (id, path) => {
        signal?.throwIfAborted();
        const node = lock.packages[id];
        const real = snapshot.layout.packagePath(id, path);
        // Past the bounds, the remaining files are only counted.
        if (snapshot.truncated) return snapshot.skip();
        return snapshot.add(real, await read(`${packageRoot}/${node.key}/${path}`));
    };
    // The lib files of the compiler first, then the declarations of the whole locked graph.
    const ordered = [typescript, ...Object.keys(lock.packages).filter(id => id !== typescript).sort()];
    for (const id of ordered)
        for (const path of lock.packages[id].files)
            if (path === 'package.json' || (id === typescript ? /^lib\/lib\..*\.d\.ts$/.test(path) : declaration.test(path)))
                await loadPackageFile(id, path);

    const attempted = new Set();
    const configText = await readOptional(read, 'tsconfig.json');
    let options;
    let rootNames;
    let errors = [];
    if (configText !== null) {
        const configSource = decoder.decode(configText);
        snapshot.add(configPath, configText);
        attempted.add(configPath);
        // The configuration and the files it extends, until no read misses.
        let parsed = parseConfig(ts, snapshot, configSource);
        for (let pass = 0; pass < maxConfigPasses && await loadMisses(snapshot, {read, lock}, attempted); ++pass)
            parsed = parseConfig(ts, snapshot, configSource);
        const directories = Object.entries(parsed.wildcardDirectories ?? {})
            .map(([directory, flags]) => [projectRelative(directory), (flags & 1) !== 0])
            .filter(([directory]) => directory !== null);
        const listed = await listProject(snapshot, list, directories, bounds, signal);
        const wanted = new Set([...listed.filter(projectSource(parsed.options)),
            ...parsed.fileNames.map(projectRelative).filter(path => path !== null)]);
        for (const path of wanted) await loadProjectFile(path);
        parsed = parseConfig(ts, snapshot, configSource);
        ({options, fileNames: rootNames, errors} = parsed);
    } else {
        const converted = ts.convertCompilerOptionsFromJson(defaultCompilerOptions, projectRoot);
        options = converted.options;
        errors = converted.errors;
        const sources = entries.filter(entry => projectExtensions(options).some(extension => entry.endsWith(extension)));
        if (!sources.length)
            throw new Error('wf check needs a tsconfig.json or TypeScript entries in webfront.project.json');
        // The directories of the entries, recursively: the files they import are found there.
        const directories = [...new Set(sources.map(entry => entry.includes('/') ? entry.slice(0, entry.lastIndexOf('/')) : ''))];
        const listed = await listProject(snapshot, list, directories.map(directory => [directory, true]), bounds, signal);
        for (const path of new Set([...sources, ...listed.filter(projectSource(options))]))
            await loadProjectFile(path);
        rootNames = sources.map(entry => `${projectRoot}/${entry}`);
    }
    snapshot.misses.clear();
    return {snapshot, rootNames, options, errors};

    async function loadProjectFile(path) {
        signal?.throwIfAborted();
        const real = `${projectRoot}/${path}`;
        attempted.add(real);
        if (snapshot.truncated) return snapshot.skip();
        const bytes = await readOptional(read, path);
        if (bytes !== null) snapshot.add(real, bytes);
    }
}

/** The CompilerHost of phase 2: synchronous, over the snapshot only; it writes nothing. */
export function createCompilerHost(ts, snapshot, typescript) {
    const libLocation = snapshot.layout.packagePath(typescript, 'lib');
    return {
        getSourceFile: (fileName, languageVersionOrOptions) => {
            const text = snapshot.readFile(fileName);
            return text === undefined ? undefined : ts.createSourceFile(fileName, text, languageVersionOrOptions);
        },
        getDefaultLibLocation: () => libLocation,
        getDefaultLibFileName: options => `${libLocation}/${ts.getDefaultLibFileName(options)}`,
        writeFile: () => {},
        getCurrentDirectory: () => projectRoot,
        getCanonicalFileName: fileName => fileName,
        useCaseSensitiveFileNames: () => true,
        getNewLine: () => '\n',
        fileExists: snapshot.fileExists,
        readFile: snapshot.readFile,
        directoryExists: snapshot.directoryExists,
        getDirectories: snapshot.getDirectories,
        realpath: snapshot.realpath,
    };
}

/** Phase 2: the program over the snapshot and all its diagnostics before emit, configuration ones first. */
export function checkProgram(ts, {snapshot, rootNames, options, errors}, typescript) {
    const host = createCompilerHost(ts, snapshot, typescript);
    const program = ts.createProgram({rootNames, options: {...options, ...noEmit}, host});
    return {program, diagnostics: [...errors, ...ts.getPreEmitDiagnostics(program)]};
}

/** A path as the terminal shows it: project-relative, or <package id>/<path> inside a package. */
export function displayPath(snapshot, path) {
    const relative = projectRelative(path);
    if (relative !== null) return relative;
    const location = snapshot.layout.locate(path);
    return location?.kind === 'package' ? `${location.id}/${location.path}` : path;
}

/** One line per diagnostic, like tsc without --pretty: file(line,column): error TScode: message. */
export function formatDiagnostic(ts, snapshot, diagnostic) {
    const category = ts.DiagnosticCategory[diagnostic.category].toLowerCase();
    const message = ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n');
    if (!diagnostic.file) return `${category} TS${diagnostic.code}: ${message}`;
    const {line, character} = diagnostic.file.getLineAndCharacterOfPosition(diagnostic.start ?? 0);
    return `${displayPath(snapshot, diagnostic.file.fileName)}(${line + 1},${character + 1}): ${category} TS${diagnostic.code}: ${message}`;
}

/** The terminal lines and exit code of a check: 1 for a type error or a truncated snapshot. */
export function checkReport(ts, snapshot, {program, diagnostics}, bounds = snapshotBounds) {
    const lines = diagnostics.map(diagnostic => formatDiagnostic(ts, snapshot, diagnostic));
    const errors = diagnostics.filter(diagnostic => diagnostic.category === ts.DiagnosticCategory.Error);
    if (snapshot.truncated)
        lines.push(`error: the snapshot reached its bounds (${bounds.files} files, ${bounds.bytes / 1048576} MiB): ` +
            `${snapshot.skipped} files were not loaded and are treated as missing`);
    const files = new Set(errors.map(diagnostic => diagnostic.file?.fileName ?? ''));
    const projectFiles = program.getSourceFiles().filter(file => projectRelative(file.fileName) !== null).length;
    if (errors.length) lines.push(`Found ${errors.length} error${errors.length === 1 ? '' : 's'} in ${files.size} file${files.size === 1 ? '' : 's'}.`);
    else if (!snapshot.truncated) lines.push(`No type errors in ${projectFiles} project file${projectFiles === 1 ? '' : 's'} (TypeScript ${ts.version}).`);
    return {lines, code: errors.length || snapshot.truncated ? 1 : 0};
}
