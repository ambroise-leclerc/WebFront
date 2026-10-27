import {createElement, useLayoutEffect, useRef, useSyncExternalStore} from 'react';

function reportFailure(message, error) {
    try { console.error(message, error); }
    catch { /* Diagnostics must not interrupt another consumer or escape effect cleanup. */ }
}

/** A cached snapshot, replaced once per C++ message. Treat bridge arrays as immutable values. */
export function createStore(name, initialSnapshot, {target = globalThis} = {}) {
    if (!/^[A-Za-z0-9_-]+$/.test(name)) throw new TypeError('Invalid React store name');
    const registry = target.webfrontReactStores ??= Object.create(null);
    if (Object.hasOwn(registry, name)) throw new Error('React store name already installed');
    let snapshot = initialSnapshot, disposed = false;
    const listeners = new Set();
    const receive = value => {
        if (Object.is(snapshot, value)) return;
        snapshot = value;
        for (const listener of [...listeners]) {
            try { listener(); }
            catch (error) { reportFailure('WebFront React store subscriber failed', error); }
        }
    };
    registry[name] = receive;
    const store = {
        getSnapshot: () => snapshot,
        subscribe(listener) {
            if (disposed) throw new Error('React store disposed');
            listeners.add(listener);
            return () => listeners.delete(listener);
        },
        dispose() {
            disposed = true;
            listeners.clear();
            if (registry[name] === receive) delete registry[name];
        },
    };
    return store;
}

export function useStore(store) {
    return useSyncExternalStore(store.subscribe, store.getSnapshot);
}

// Defer release through StrictMode's synchronous cleanup/setup replay. Real unmounts still revoke.
const leases = new WeakMap();
function releaseLease(names, name, lease) {
    try { lease.release(); }
    catch (error) { reportFailure('WebFront React island release failed', error); }
    finally { if (names.get(name) === lease) names.delete(name); }
}

export function Island({dom, name, children, dangerouslySetInnerHTML, ...props}) {
    if (children != null || dangerouslySetInnerHTML != null) throw new Error('Island must be rendered without children');
    const element = useRef(null);
    const identity = useRef({dom, name});
    if (identity.current.dom !== dom || identity.current.name !== name)
        throw new Error('Island dom and name must stay stable; remount with a key');
    useLayoutEffect(() => {
        let names = leases.get(dom);
        if (!names) leases.set(dom, names = new Map());
        let lease = names.get(name);
        if (lease?.pending && lease.element !== element.current) {
            releaseLease(names, name, lease);
            lease = null;
        }
        if (lease && (!lease.pending || lease.element !== element.current)) throw new Error('Island name already mounted');
        if (!lease) {
            lease = {element: element.current, release: dom.island(name, element.current), pending: false};
            names.set(name, lease);
        }
        lease.pending = false;
        return () => {
            lease.pending = true;
            queueMicrotask(() => {
                if (lease.pending && names.get(name) === lease) {
                    releaseLease(names, name, lease);
                }
            });
        };
    }, [dom, name]);
    return createElement('div', {...props, ref: element});
}
