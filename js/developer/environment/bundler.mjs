// Virtual project/npm filesystem for esbuild's browser API. No Node.js shims or network imports.
import {normalize, packageEntry} from './entry.mjs';
import {resolveImport} from './resolve.mjs';
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

function createFinder(packages, readProject, loaded) {
    const read = async (namespace, path) => {
        if (namespace === 'project') return readProject(path);
        const slash = path.indexOf('/');
        return packages[path.slice(0, slash)]?.get(path.slice(slash + 1));
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
    const {read, find} = createFinder(packages, readProject, loaded);
    const load = namespace => async args => {
        const bytes = loaded.get(`${namespace}:${args.path}`) ?? await read(namespace, args.path);
        const loader = loaders.get(args.path.split('.').pop());
        return {contents: loader ? decoder.decode(bytes) : bytes, loader: loader ?? 'file'};
    };
    return {name: 'webfront-project', setup(build) {
        build.onResolve({filter: /.*/}, args => resolveImport(args, lock, find));
        build.onLoad({filter: /.*/, namespace: 'empty'}, () => ({contents: '', loader: 'js'}));
        for (const namespace of ['project', 'npm']) build.onLoad({filter: /.*/, namespace}, load(namespace));
    }};
}

export async function bundle(esbuild, {entry, lock, packages, readProject}) {
    const result = await esbuild.build({entryPoints: [entry], bundle: true, write: false,
        outdir: '/output', entryNames: 'app', assetNames: 'assets/[name]-[hash]',
        format: 'esm', platform: 'browser', target: 'es2020', jsx: 'automatic', minify: true,
        define: {'process.env.NODE_ENV': '"production"'}, legalComments: 'inline',
        plugins: [virtualPlugin(lock, packages, readProject)]});
    return new Map(result.outputFiles.map(file => [file.path.replace(/^\/output\//, ''), file.contents]));
}

// Node.js built-in modules: a script importing one fails while it is loaded, unless a locked package of that name
// (an npm polyfill such as "events") is a dependency of the importer.
const nodeBuiltins = new Set(['assert', 'async_hooks', 'buffer', 'child_process', 'cluster', 'console', 'constants', 'crypto',
    'dgram', 'diagnostics_channel', 'dns', 'domain', 'events', 'fs', 'http', 'http2', 'https', 'inspector', 'module', 'net',
    'os', 'path', 'perf_hooks', 'process', 'punycode', 'querystring', 'readline', 'repl', 'stream', 'string_decoder', 'sys',
    'timers', 'tls', 'trace_events', 'tty', 'url', 'util', 'v8', 'vm', 'wasi', 'worker_threads', 'zlib']);

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
        build.onResolve({filter: /^(?:node:|[a-z_]+(?:\/|$))/}, async args => {
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
    const {find} = createFinder(packages, readProject, loaded);
    const result = await esbuild.build({entryPoints: ['webfront-script-entry'], bundle: true, write: false,
        outfile: '/output/script.js', format: 'esm', platform: 'browser', target: 'es2022', legalComments: 'inline',
        define: {'process.env.NODE_ENV': '"production"'},
        plugins: [scriptPlugin(entry, lock, find), virtualPluginWith(lock, packages, readProject, loaded)]});
    return new Map(result.outputFiles.map(file => [file.path.replace(/^\/output\//, ''), file.contents]));
}
