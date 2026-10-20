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
    const loaded = new Map();
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
