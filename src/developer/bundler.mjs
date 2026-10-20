// Virtual project/npm filesystem for esbuild's browser API. No Node.js shims or network imports.
const decoder = new TextDecoder();
export function normalize(path) {
    const parts = [];
    for (const part of path.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') {
            if (!parts.length) throw new Error('Import escapes its source root');
            parts.pop();
        } else parts.push(part);
    }
    return parts.join('/');
}
const dirname = path => path.split('/').slice(0, -1).join('/');
const extensions = ['', '.tsx', '.ts', '.jsx', '.js', '.mjs', '.cjs', '.json', '.css', '/index.tsx', '/index.ts', '/index.js'];
function conditional(value, wildcard, conditions) {
    if (typeof value === 'string') return value.replaceAll('*', wildcard ?? '');
    if (Array.isArray(value)) {
        for (const candidate of value) {
            const result = conditional(candidate, wildcard, conditions);
            if (result) return result;
        }
    } else if (value && typeof value === 'object') {
        for (const [key, target] of Object.entries(value))
            if (conditions.has(key)) return conditional(target, wildcard, conditions);
    }
    return null;
}

export function packageEntry(manifest, subpath = '', kind = 'import') {
    const conditions = new Set(['browser', kind, 'default', 'production']);
    if (manifest.exports !== undefined) {
        const key = subpath ? `./${subpath}` : '.';
        let value = manifest.exports;
        if (value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).some(key => key.startsWith('.'))) {
            if (Object.hasOwn(value, key)) value = value[key];
            else {
                const pattern = Object.keys(value).filter(candidate => candidate.includes('*'))
                    .sort((a, b) => b.length - a.length).find(candidate => {
                        const [before, after] = candidate.split('*');
                        return key.startsWith(before) && key.endsWith(after);
                    });
                if (!pattern) throw new Error(`Package does not export ${key}`);
                const [before, after] = pattern.split('*');
                const result = conditional(value[pattern], key.slice(before.length, key.length - after.length), conditions);
                if (!result?.startsWith('./')) throw new Error(`Unsupported package export: ${key}`);
                return normalize(result);
            }
        } else if (subpath) throw new Error(`Package does not export ${key}`);
        const result = conditional(value, undefined, conditions);
        if (!result?.startsWith('./')) throw new Error(`Unsupported package export: ${key}`);
        return normalize(result);
    }
    return normalize(subpath || (typeof manifest.browser === 'string' && manifest.browser) || manifest.module || manifest.main || 'index.js');
}

export function virtualPlugin(lock, packages, readProject) {
    const loaded = new Map();
    const read = async (namespace, path) => {
        if (namespace === 'project') return readProject(path);
        const slash = path.indexOf('/');
        return packages[path.slice(0, slash)]?.get(path.slice(slash + 1));
    };
    const find = async (namespace, path) => {
        for (const suffix of extensions) {
            try {
                const content = await read(namespace, path + suffix);
                if (content !== undefined) {
                    loaded.set(`${namespace}:${path + suffix}`, content);
                    return {path: path + suffix, namespace};
                }
            } catch (error) {
                if (namespace !== 'project') throw error;
            }
        }
        throw new Error(`Cannot resolve ${path}`);
    };
    return {name: 'webfront-project', setup(build) {
        build.onResolve({filter: /.*/}, async args => {
            if (args.kind === 'entry-point') return find('project', normalize(args.path));
            let specifier = args.path;
            const importerNode = args.namespace === 'npm' ? lock.packages[args.importer.split('/')[0]] : null;
            const browser = importerNode?.manifest.browser;
            if (browser && typeof browser === 'object') {
                const relative = specifier.startsWith('.') ? './' + normalize(dirname(args.importer.split('/').slice(1).join('/')) + '/' + specifier) : specifier;
                const mapped = browser[relative];
                if (mapped === false) return {path: specifier, namespace: 'empty'};
                if (typeof mapped === 'string') {
                    if (mapped.startsWith('.')) return find('npm', `${args.importer.split('/')[0]}/${normalize(mapped)}`);
                    specifier = mapped;
                }
            }
            if (specifier.startsWith('.')) {
                if (args.namespace === 'npm') {
                    const slash = args.importer.indexOf('/');
                    const id = args.importer.slice(0, slash);
                    const path = normalize(dirname(args.importer.slice(slash + 1)) + '/' + specifier);
                    return find('npm', `${id}/${path}`);
                }
                return find('project', normalize(dirname(args.importer) + '/' + specifier));
            }
            if (/^(?:[a-z]+:|\/|#)/i.test(specifier)) throw new Error(`Unsupported browser import: ${specifier}`);
            const parts = specifier.split('/');
            const name = parts.splice(0, specifier.startsWith('@') ? 2 : 1).join('/');
            const id = (importerNode?.name === name ? args.importer.split('/')[0] : importerNode?.dependencies[name]) ?? (!importerNode && lock.roots[name]?.id);
            if (!id) throw new Error(`Uninstalled browser dependency: ${specifier}`);
            return find('npm', `${id}/${packageEntry(lock.packages[id].manifest, parts.join('/'), args.kind.startsWith('require') ? 'require' : 'import')}`);
        });
        build.onLoad({filter: /.*/, namespace: 'empty'}, () => ({contents: '', loader: 'js'}));
        for (const namespace of ['project', 'npm']) build.onLoad({filter: /.*/, namespace}, async args => {
            const bytes = loaded.get(`${namespace}:${args.path}`) ?? await read(namespace, args.path);
            const ext = args.path.split('.').pop();
            const loader = {mjs: 'js', cjs: 'js', js: 'js', jsx: 'jsx', ts: 'ts', tsx: 'tsx', css: 'css', json: 'json'}[ext];
            return {contents: loader ? decoder.decode(bytes) : bytes, loader: loader ?? 'file'};
        });
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
