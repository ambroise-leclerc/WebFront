// Virtual project/npm filesystem for esbuild's browser API. No Node.js shims or network imports.
import {normalize, packageEntry} from './entry.mjs';
import {resolveImport} from './resolve.mjs';
import {packageRoot} from './paths.mjs';
export {normalize, packageEntry};

const decoder = new TextDecoder();

const extensions = ['', '.tsx', '.ts', '.jsx', '.js', '.mjs', '.cjs', '.json', '.css', '/index.tsx', '/index.ts', '/index.js'];
const loaders = new Map(Object.entries({mjs: 'js', cjs: 'js', js: 'js', jsx: 'jsx', ts: 'ts', tsx: 'tsx', css: 'css', json: 'json'}));

// A file that does not exist is a miss in the project, but an error inside a locked package.
async function probe(read, namespace, candidate) {
    try { return await read(namespace, candidate); }
    catch (error) {
        if (namespace !== 'project') throw error;
        return undefined;
    }
}

// Package files come from `packages` (id -> path -> bytes) or, without it, are read from the cache when resolved.
export function createFinder(packages, readProject, loaded, lock) {
    const files = new Map();
    const cached = (id, path) => {
        const node = lock?.packages?.[id];
        if (!node) return undefined;
        if (!files.has(id)) files.set(id, new Set(node.files));
        return files.get(id).has(path) ? readProject(`${packageRoot}/${node.key}/${path}`) : undefined;
    };
    const read = async (namespace, path) => {
        if (namespace === 'project') return readProject(path);
        const slash = path.indexOf('/');
        const [id, file] = [path.slice(0, slash), path.slice(slash + 1)];
        return packages ? packages[id]?.get(file) : cached(id, file);
    };
    const find = async (namespace, path) => {
        for (const suffix of extensions) {
            const content = await probe(read, namespace, path + suffix);
            if (content === undefined) continue;
            loaded.set(`${namespace}:${path + suffix}`, content);
            return {path: path + suffix, namespace};
        }
        throw new Error(`Cannot resolve ${path}`);
    };
    return {read, find};
}

export function virtualPlugin(lock, packages, readProject) {
    return virtualPluginWith(lock, packages, readProject, new Map());
}

function virtualPluginWith(lock, packages, readProject, loaded) {
    const {read, find} = createFinder(packages, readProject, loaded, lock);
    const load = namespace => async args => {
        const bytes = loaded.get(`${namespace}:${args.path}`) ?? await read(namespace, args.path);
        const loader = loaders.get(args.path.split('.').pop());
        return {contents: loader ? decoder.decode(bytes) : bytes, loader: loader ?? 'file'};
    };
    return {name: 'webfront-project', setup(build) {
        // Resolution caches belong to one rebuild: source bytes must never survive into the next one.
        build.onStart?.(() => loaded.clear());
        build.onResolve({filter: /.*/}, args => resolveImport(args, lock, find));
        build.onLoad({filter: /.*/, namespace: 'empty'}, () => ({contents: '', loader: 'js'}));
        for (const namespace of ['project', 'npm']) build.onLoad({filter: /.*/, namespace}, load(namespace));
    }};
}

function applicationOptions({entry, lock, packages, readProject}) {
    return {entryPoints: [entry], bundle: true, write: false,
        outdir: '/output', entryNames: 'app', assetNames: 'assets/[name]-[hash]',
        format: 'esm', platform: 'browser', target: 'es2020', jsx: 'automatic', minify: true,
        define: {'process.env.NODE_ENV': '"production"'}, legalComments: 'inline',
        plugins: [virtualPlugin(lock, packages, readProject)]};
}

const outputFiles = result => new Map(result.outputFiles.map(file => [file.path.replace(/^\/output\//, ''), file.contents]));

export async function bundle(esbuild, options) {
    return outputFiles(await esbuild.build(applicationOptions(options)));
}

/** One esbuild context for repeated builds; the virtual filesystem is read again on every rebuild. */
export async function bundleContext(esbuild, options) {
    const context = await esbuild.context(applicationOptions(options));
    return {rebuild: async () => outputFiles(await context.rebuild()), dispose: () => context.dispose()};
}

// Node.js built-in modules: a script importing one fails while it is loaded, unless a locked package of that name
// (an npm polyfill such as "events") is a dependency of the importer.
const nodeBuiltins = new Set(['assert', 'async_hooks', 'buffer', 'child_process', 'cluster', 'console', 'constants', 'crypto',
    'dgram', 'diagnostics_channel', 'dns', 'domain', 'events', 'fs', 'http', 'http2', 'https', 'inspector', 'module', 'net',
    'os', 'path', 'perf_hooks', 'process', 'punycode', 'querystring', 'readline', 'repl', 'stream', 'string_decoder', 'sys',
    'timers', 'tls', 'trace_events', 'tty', 'url', 'util', 'v8', 'vm', 'wasi', 'worker_threads', 'zlib']);

// Every specifier that may name a built-in module, digits included (http2, v8).
export const nodeImport = /^(?:node:|[a-z_][a-z0-9_]*(?:\/|$))/;

export function nodeBuiltin(specifier) {
    if (specifier.startsWith('node:')) return specifier.slice(5).split('/')[0];
    const name = specifier.split('/')[0];
    return nodeBuiltins.has(name) ? name : null;
}

const nodeError = name => `"${name}" is a Node.js API, which WebFront scripts do not provide (no fs, child_process or process; use the context API)`;

// The script's entry is given as {namespace, path}: a project module, or a file of a locked package for wf exec.
function scriptPlugin(entry, lock, find) {
    return {name: 'webfront-script', setup(build) {
        build.onResolve({filter: /^webfront-script-entry$/}, () => find(entry.namespace, entry.path));
        build.onResolve({filter: nodeImport}, async args => {
            const name = nodeBuiltin(args.path);
            if (!name) return undefined;
            if (args.path.startsWith('node:')) return {errors: [{text: nodeError(name)}]};
            try { return await resolveImport(args, lock, find); }
            catch { return {errors: [{text: nodeError(name)}]}; }
        });
    }};
}

/** One ES module for a script Worker: TS/JS of the project or of a locked package, with the locked dependencies. */
export async function bundleScript(esbuild, {entry, lock, packages, readProject}) {
    const loaded = new Map();
    const {find} = createFinder(packages, readProject, loaded, lock);
    const result = await esbuild.build({entryPoints: ['webfront-script-entry'], bundle: true, write: false,
        outfile: '/output/script.js', format: 'esm', platform: 'browser', target: 'es2022', jsx: 'automatic', legalComments: 'inline',
        define: {'process.env.NODE_ENV': '"production"'},
        plugins: [scriptPlugin(entry, lock, find), virtualPluginWith(lock, packages, readProject, loaded)]});
    return new Map(result.outputFiles.map(file => [file.path.replace(/^\/output\//, ''), file.contents]));
}

/**
 * The specs of `wf test` as one ES module: the project's spec files imported in the manifest's order, with the locked
 * dependencies. The Jasmine functions are globals of the runner page, so no spec imports jasmine-core.
 */
export async function bundleTests(esbuild, {specs, lock, packages, readProject}) {
    const loaded = new Map();
    const {find} = createFinder(packages, readProject, loaded, lock);
    const contents = specs.map(path => `import ${JSON.stringify(`webfront-spec:${path}`)};`).join('\n');
    const entry = {name: 'webfront-tests', setup(build) {
        build.onResolve({filter: /^webfront-test-entry$/}, () => ({path: 'entry', namespace: 'webfront-tests'}));
        build.onLoad({filter: /.*/, namespace: 'webfront-tests'}, () => ({contents, loader: 'js'}));
        build.onResolve({filter: /^webfront-spec:/}, args => find('project', normalize(args.path.slice('webfront-spec:'.length))));
    }};
    const result = await esbuild.build({entryPoints: ['webfront-test-entry'], bundle: true, write: false,
        outfile: '/output/specs.js', format: 'esm', platform: 'browser', target: 'es2022', jsx: 'automatic',
        define: {'process.env.NODE_ENV': '"test"'}, legalComments: 'inline',
        plugins: [entry, virtualPluginWith(lock, packages, readProject, loaded)]});
    return new Map(result.outputFiles.map(file => [file.path.replace(/^\/output\//, ''), file.contents]));
}
