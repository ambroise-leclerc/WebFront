// Virtual project/npm filesystem for esbuild's browser API. No Node.js shims or network imports.
const decoder = new TextDecoder();

function applySegment(parts, part) {
    if (!part || part === '.') return;
    if (part !== '..') {
        parts.push(part);
        return;
    }
    if (!parts.length) throw new Error('Import escapes its source root');
    parts.pop();
}

export function normalize(path) {
    const parts = [];
    for (const part of path.split('/')) applySegment(parts, part);
    return parts.join('/');
}
const dirname = path => path.split('/').slice(0, -1).join('/');
const extensions = ['', '.tsx', '.ts', '.jsx', '.js', '.mjs', '.cjs', '.json', '.css', '/index.tsx', '/index.ts', '/index.js'];
const loaders = new Map(Object.entries({mjs: 'js', cjs: 'js', js: 'js', jsx: 'jsx', ts: 'ts', tsx: 'tsx', css: 'css', json: 'json'}));

function isPlainObject(value) {
    if (!value || typeof value !== 'object') return false;
    return !Array.isArray(value);
}

function firstMatch(candidates, wildcard, conditions) {
    for (const candidate of candidates) {
        const result = conditional(candidate, wildcard, conditions);
        if (result) return result;
    }
    return null;
}

function byCondition(object, wildcard, conditions) {
    for (const [key, target] of Object.entries(object))
        if (conditions.has(key)) return conditional(target, wildcard, conditions);
    return null;
}

function conditional(value, wildcard, conditions) {
    if (typeof value === 'string') return value.replaceAll('*', wildcard ?? '');
    if (Array.isArray(value)) return firstMatch(value, wildcard, conditions);
    if (isPlainObject(value)) return byCondition(value, wildcard, conditions);
    return null;
}

function exportedPath(result, key) {
    if (!result?.startsWith('./')) throw new Error(`Unsupported package export: ${key}`);
    return normalize(result);
}

function isSubpathMap(value) {
    if (!isPlainObject(value)) return false;
    return Object.keys(value).some(key => key.startsWith('.'));
}

function patternEntry(exports, key, conditions) {
    const pattern = Object.keys(exports).filter(candidate => candidate.includes('*'))
        .sort((a, b) => b.length - a.length).find(candidate => {
            const [before, after] = candidate.split('*');
            return key.startsWith(before) && key.endsWith(after);
        });
    if (!pattern) throw new Error(`Package does not export ${key}`);
    const [before, after] = pattern.split('*');
    return exportedPath(conditional(exports[pattern], key.slice(before.length, key.length - after.length), conditions), key);
}

function legacyMain(manifest) {
    if (typeof manifest.browser === 'string' && manifest.browser) return manifest.browser;
    return manifest.module || manifest.main || 'index.js';
}

export function packageEntry(manifest, subpath = '', kind = 'import') {
    const conditions = new Set(['browser', kind, 'default', 'production']);
    const exports = manifest.exports;
    if (exports === undefined) return normalize(subpath || legacyMain(manifest));
    const key = subpath ? `./${subpath}` : '.';
    if (!isSubpathMap(exports)) {
        if (subpath) throw new Error(`Package does not export ${key}`);
        return exportedPath(conditional(exports, undefined, conditions), key);
    }
    if (Object.hasOwn(exports, key)) return exportedPath(conditional(exports[key], undefined, conditions), key);
    return patternEntry(exports, key, conditions);
}

const packageId = importer => importer.split('/')[0];
const packagePath = importer => importer.slice(importer.indexOf('/') + 1);

// The package.json "browser" object may redirect, hide or replace a specifier of the importing package.
function applyBrowserField(importerNode, args) {
    const browser = importerNode?.manifest.browser;
    if (!isPlainObject(browser)) return {specifier: args.path};
    const relative = args.path.startsWith('.') ? './' + normalize(dirname(packagePath(args.importer)) + '/' + args.path) : args.path;
    const mapped = browser[relative];
    if (mapped === false) return {empty: true, specifier: args.path};
    if (typeof mapped !== 'string') return {specifier: args.path};
    if (mapped.startsWith('.')) return {local: `${packageId(args.importer)}/${normalize(mapped)}`, specifier: args.path};
    return {specifier: mapped};
}

function resolveRelative(args, specifier, find) {
    if (args.namespace !== 'npm') return find('project', normalize(dirname(args.importer) + '/' + specifier));
    return find('npm', `${packageId(args.importer)}/${normalize(dirname(packagePath(args.importer)) + '/' + specifier)}`);
}

function dependencyId(args, name, importerNode, lock) {
    if (!importerNode) return lock.roots[name]?.id;
    return importerNode.name === name ? packageId(args.importer) : importerNode.dependencies[name];
}

function resolveBare(args, specifier, importerNode, lock, find) {
    const parts = specifier.split('/');
    const name = parts.splice(0, specifier.startsWith('@') ? 2 : 1).join('/');
    const id = dependencyId(args, name, importerNode, lock);
    if (!id) throw new Error(`Uninstalled browser dependency: ${specifier}`);
    const kind = args.kind.startsWith('require') ? 'require' : 'import';
    return find('npm', `${id}/${packageEntry(lock.packages[id].manifest, parts.join('/'), kind)}`);
}

async function resolveImport(args, lock, find) {
    if (args.kind === 'entry-point') return find('project', normalize(args.path));
    const importerNode = args.namespace === 'npm' ? lock.packages[packageId(args.importer)] : null;
    const browser = applyBrowserField(importerNode, args);
    if (browser.empty) return {path: browser.specifier, namespace: 'empty'};
    if (browser.local) return find('npm', browser.local);
    if (browser.specifier.startsWith('.')) return resolveRelative(args, browser.specifier, find);
    if (/^(?:[a-z]+:|\/|#)/i.test(browser.specifier)) throw new Error(`Unsupported browser import: ${browser.specifier}`);
    return resolveBare(args, browser.specifier, importerNode, lock, find);
}

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
