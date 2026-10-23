// Helpers of the developer specs (run by `wf test`): the assertions that Jasmine has no matcher for, and the SHA-512
// integrity of the registries they fake. Only browser APIs.
//
// rejects(promise, expected) and throws(callback, expected) check that the promise rejects or the callback throws, then
// that the error satisfies `expected`: a RegExp tested against String(error), a function returning true for the error,
// or nothing.
function satisfies(error, expected) {
    if (expected === undefined) return;
    if (expected instanceof RegExp) expect(String(error)).toMatch(expected);
    else if (typeof expected === 'function') expect(expected(error)).withContext(`validation of ${error}`).toBeTrue();
    else if (typeof expected === 'string') return;
    else throw new TypeError('An expected error is a RegExp, a function or nothing');
}

export async function rejects(promise, expected) {
    try {
        await (typeof promise === 'function' ? promise() : promise);
    } catch (error) {
        satisfies(error, expected);
        return;
    }
    fail('Missing expected rejection');
}

export function throws(callback, expected) {
    try {
        callback();
    } catch (error) {
        satisfies(error, expected);
        return;
    }
    fail('Missing expected exception');
}

/** The SRI value of an npm integrity field. */
export async function integrity(bytes) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-512', bytes));
    return 'sha512-' + btoa(String.fromCharCode(...digest));
}

/** The lowercase hexadecimal SHA-256 or SHA-512 of some bytes. */
export async function hexDigest(algorithm, bytes) {
    return Array.from(new Uint8Array(await crypto.subtle.digest(algorithm, bytes)), byte => byte.toString(16).padStart(2, '0')).join('');
}

// The specs run in the page of `wf test`, whose bridge reaches the project of the application: they can read any file of
// the project, the sources they test included.
import {readProject} from '../../js/developer/environment/io.mjs';
import * as esbuildWasm from 'esbuild-wasm';

/** The bytes of a project file. */
export const projectBytes = path => readProject(webFront, path);

/** The text of a project file. */
export async function projectText(path) {
    return new TextDecoder().decode(await projectBytes(path));
}

const relativeImport = /(\bfrom\s*|\bimport\s*)(['"])(\.{1,2}\/[^'"]+)\2/g;
const resolved = (from, specifier) => {
    const parts = from.split('/').slice(0, -1);
    for (const part of specifier.split('/')) {
        if (part === '..') parts.pop();
        else if (part !== '.') parts.push(part);
    }
    return parts.join('/');
};

/**
 * A fresh evaluation of a module of the project (its relative imports too), as the URL of a Blob: each call gives the
 * module state of its own, for modules that keep state at their top level, like a Worker's. The sources are read
 * through the bridge, so the module is the one of the project, not a copy of it.
 */
export async function freshModule(path, loading = new Map()) {
    if (loading.has(path)) return loading.get(path);
    let source = await projectText(path);
    const dependencies = new Map();
    for (const [, , , specifier] of source.matchAll(relativeImport)) {
        const dependency = resolved(path, specifier);
        dependencies.set(specifier, await freshModule(dependency, loading));
    }
    source = source.replace(relativeImport, (_, keyword, quote, specifier) => `${keyword}${quote}${dependencies.get(specifier)}${quote}`);
    const url = URL.createObjectURL(new Blob([source], {type: 'text/javascript'}));
    loading.set(path, url);
    return url;
}

let initialized;
/**
 * The esbuild-wasm of the project's lock, initialized once for the page: the compiler the environment itself uses.
 */
export function esbuild() {
    initialized ??= (async () => {
        const lock = JSON.parse(await projectText('webfront-packages.lock.json'));
        const key = lock.packages[lock.roots['esbuild-wasm'].id].key;
        const wasmModule = await WebAssembly.compile(await projectBytes(`.webfront/developer/packages/${key}/esbuild.wasm`));
        await esbuildWasm.initialize({wasmModule, worker: false});
        return esbuildWasm;
    })();
    return initialized;
}
