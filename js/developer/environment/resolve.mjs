// Import resolution for the virtual project/npm filesystem: relative, bare and "browser" field specifiers.
import {isPlainObject, normalize, packageEntry} from './entry.mjs';

const dirname = path => path.split('/').slice(0, -1).join('/');
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

function resolveBare(args, specifier, importerNode, {lock, find}) {
    const parts = specifier.split('/');
    const name = parts.splice(0, specifier.startsWith('@') ? 2 : 1).join('/');
    const id = dependencyId(args, name, importerNode, lock);
    if (!id) throw new Error(`Uninstalled browser dependency: ${specifier}`);
    const kind = args.kind.startsWith('require') ? 'require' : 'import';
    return find('npm', `${id}/${packageEntry(lock.packages[id].manifest, parts.join('/'), kind)}`);
}

export async function resolveImport(args, lock, find) {
    if (args.kind === 'entry-point') return find('project', normalize(args.path));
    const importerNode = args.namespace === 'npm' ? lock.packages[packageId(args.importer)] : null;
    const browser = applyBrowserField(importerNode, args);
    if (browser.empty) return {path: browser.specifier, namespace: 'empty'};
    if (browser.local) return find('npm', browser.local);
    if (browser.specifier.startsWith('.')) return resolveRelative(args, browser.specifier, find);
    if (/^(?:[a-z]+:|\/|#)/i.test(browser.specifier)) throw new Error(`Unsupported browser import: ${browser.specifier}`);
    return resolveBare(args, browser.specifier, importerNode, {lock, find});
}
