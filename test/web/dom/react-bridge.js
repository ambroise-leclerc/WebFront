// ADR-018 (#378): throwaway React integration of the DOM prototype, never part of the library.
//  - loadReact() evaluates the CommonJS production builds of react, react-dom and scheduler served under /packages/
//    (a real application gets them from wf, which bundles them; this page has no build step).
//  - wfStore is an external store that C++ writes (ui.jsFunction("wfStore.set")(key, value)) and React reads with
//    useSyncExternalStore: React keeps owning its DOM, C++ only provides values.
//  - Island lends an element React renders without children to wfDom: C++ owns that element's children.
"use strict";
(function (global) {
    const sources = {
        "react": "/packages/react/cjs/react.production.js",
        "scheduler": "/packages/scheduler/cjs/scheduler.production.js",
        "react-dom": "/packages/react-dom/cjs/react-dom.production.js",
        "react-dom/client": "/packages/react-dom/cjs/react-dom-client.production.js",
    };

    async function loadReact() {
        const texts = Object.fromEntries(await Promise.all(Object.entries(sources).map(async ([name, url]) => {
            const response = await fetch(url);
            if (!response.ok) throw new Error(`cannot load ${url}: ${response.status}`);
            return [name, await response.text()];
        })));
        const cache = new Map();
        const load = name => {
            if (cache.has(name)) return cache.get(name).exports;
            if (!(name in texts)) throw new Error(`module ${name} is not available`);
            const module = {exports: {}};
            cache.set(name, module);
            // The CommonJS wrapper: the builds read their dependencies through the third argument.
            new Function("module", "exports", "require", "process", texts[name])(module, module.exports, load,
                                                                                 {env: {NODE_ENV: "production"}});
            return module.exports;
        };
        return {React: load("react"), ReactDOMClient: load("react-dom/client")};
    }

    // Values by key; a snapshot is the value itself (a number or a string), so it is immutable as React requires.
    const values = new Map();
    const subscribers = new Set();
    const wfStore = {
        set(key, value) {
            if (Object.is(values.get(key), value)) return;
            values.set(key, value);
            for (const notify of subscribers) notify();
        },
        get: key => values.get(key),
        subscribe(notify) {
            subscribers.add(notify);
            return () => subscribers.delete(notify);
        },
    };

    function bind(React) {
        const h = React.createElement;
        const useCppValue = (key, fallback) =>
            React.useSyncExternalStore(wfStore.subscribe, () => values.has(key) ? values.get(key) : fallback);
        // The element is rendered without children: React never reconciles what C++ appends inside it.
        function Island({name, className}) {
            const ref = React.useRef(null);
            React.useLayoutEffect(() => global.wfDom.island(name, ref.current), [name]);
            return h("div", {ref, className, "data-island": name});
        }
        return {h, useCppValue, Island};
    }

    global.wfStore = wfStore;
    global.reactBridge = {loadReact, bind};
})(globalThis);
