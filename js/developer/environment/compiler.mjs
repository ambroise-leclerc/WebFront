import * as esbuild from 'esbuild-wasm';
import {bundle, bundleScript, bundleContext} from './bundler.mjs';

const pending = new Map();
let nextId = 0;
const readProject = path => new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, {resolve, reject});
    self.postMessage({type: 'read', id, path});
});
const initialized = esbuild.initialize({wasmURL: new URL('./esbuild.wasm', import.meta.url).href, worker: false});
let context, contextKey;
self.onmessage = async ({data}) => {
    if (data.type === 'read-result') {
        const request = pending.get(data.id);
        if (!request) return;
        pending.delete(data.id);
        if (data.error) request.reject(new Error(data.error));
        else request.resolve(data.bytes);
        return;
    }
    if (data.type !== 'build' && data.type !== 'script') return;
    try {
        await initialized;
        let files;
        if (data.incremental) {
            const key = JSON.stringify([data.entry, data.lock]);
            if (key !== contextKey) {
                await context?.dispose();
                contextKey = undefined;
                context = await bundleContext(esbuild, {...data, readProject});
                contextKey = key;
            }
            files = await context.rebuild();
        } else files = await (data.type === 'script' ? bundleScript : bundle)(esbuild, {...data, readProject});
        self.postMessage({type: 'result', files});
    } catch (error) {
        self.postMessage({type: 'error', error: error.message, diagnostics: error.errors?.map(({text, location}) => ({text, location}))});
    }
};
