// The adapter is resolved from the package installed by wf, with development React for StrictMode replay.
import React, {StrictMode, createElement, useLayoutEffect} from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import {Island, createStore, useStore} from '@webfront/react';

const until = async predicate => {
    const deadline = performance.now() + 5000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error('React adapter condition timed out');
        await new Promise(resolve => setTimeout(resolve, 5));
    }
};
async function page() {
    const frame = document.createElement('iframe');
    frame.src = '/react.html';
    document.body.appendChild(frame);
    await until(() => frame.contentWindow?.dom);
    await frame.contentWindow.webFront.ready;
    return frame;
}
const call = (frame, name, ...args) => frame.contentWindow.webFront.cppFunction(name)(...args);

describe('Optional wf React adapter', () => {
    it('caches snapshots, unsubscribes, disposes and rejects duplicate names', () => {
        const store = createStore('local', [0]);
        try {
            expect(store.getSnapshot()).toBe(store.getSnapshot());
            expect(() => createStore('local', 0)).toThrow();
            expect(() => createStore('invalid.name', 0)).toThrow();
            let notifications = 0;
            const off = store.subscribe(() => ++notifications);
            const next = [1];
            globalThis.webfrontReactStores.local(next);
            globalThis.webfrontReactStores.local(next);
            expect(store.getSnapshot()).toBe(next);
            expect(notifications).toBe(1);
            off();
            globalThis.webfrontReactStores.local([2]);
            expect(notifications).toBe(1);
        } finally { store.dispose(); }
        expect(Object.hasOwn(globalThis.webfrontReactStores, 'local')).toBeFalse();
        expect(() => store.subscribe(() => {})).toThrow();
        createStore('local', 0).dispose();
    });

    it('isolates throwing subscribers, including failures in diagnostic logging', () => {
        const store = createStore('isolated', 0);
        const failure = new Error('subscriber failed');
        const later = jasmine.createSpy('later subscriber');
        const diagnostic = spyOn(console, 'error');
        store.subscribe(() => { throw failure; });
        store.subscribe(later);
        try {
            expect(() => globalThis.webfrontReactStores.isolated(1)).not.toThrow();
            expect(later).toHaveBeenCalledTimes(1);
            expect(diagnostic).toHaveBeenCalledWith('WebFront React store subscriber failed', failure);
            diagnostic.and.throwError('logging failed');
            expect(() => globalThis.webfrontReactStores.isolated(2)).not.toThrow();
            expect(later).toHaveBeenCalledTimes(2);
            expect(store.getSnapshot()).toBe(2);
        } finally { store.dispose(); }
    });

    it('contains an unexpected synchronous island release error and forgets the local loan', async () => {
        const container = document.body.appendChild(document.createElement('div'));
        const failure = new Error('release failed');
        const diagnostic = spyOn(console, 'error');
        const dom = {island: () => () => { throw failure; }};
        let root = createRoot(container);
        try {
            flushSync(() => root.render(createElement(Island, {dom, name: 'throwing-panel'})));
            root.unmount(); root = null;
            await Promise.resolve();
            expect(diagnostic).toHaveBeenCalledWith('WebFront React island release failed', failure);
            root = createRoot(container);
            flushSync(() => root.render(createElement(Island, {dom, name: 'throwing-panel'})));
            root.unmount(); root = null;
            await Promise.resolve();
        } finally {
            root?.unmount();
            await Promise.resolve();
            container.remove();
        }
    });

    it('unmounts a real island after its bridge disconnects without an unhandled error', async () => {
        const frame = await page(), pageWindow = frame.contentWindow;
        const dom = pageWindow.dom;
        const root = createRoot(frame.contentDocument.getElementById('root'));
        const diagnostics = [], errors = [];
        const previous = pageWindow.console.error;
        pageWindow.console.error = (...args) => diagnostics.push(args);
        const unexpected = event => { errors.push(event.type); event.preventDefault(); };
        pageWindow.addEventListener('error', unexpected);
        pageWindow.addEventListener('unhandledrejection', unexpected);
        let unmounted = false;
        try {
            flushSync(() => root.render(createElement(Island, {dom, name: 'panel'})));
            await dom.connect();
            await call(frame, 'reactPanel');
            await until(() => frame.contentDocument.getElementById('root').textContent === 'C++ island');
            await pageWindow.disconnectBridge();
            root.unmount(); unmounted = true;
            await until(() => diagnostics.some(args => args[0] === 'WebFront DOM island revocation failed'));
            await new Promise(resolve => setTimeout(resolve, 0));
            expect(errors).toEqual([]);
        } finally {
            if (!unmounted) root.unmount();
            await Promise.resolve();
            pageWindow.console.error = previous;
            pageWindow.removeEventListener('error', unexpected);
            pageWindow.removeEventListener('unhandledrejection', unexpected);
            frame.remove();
        }
    });

    it('receives one snapshot and one React commit for 1000 C++ updates', async () => {
        const frame = await page();
        const store = createStore('counter', [0, 'initial'], {target: frame.contentWindow});
        const root = createRoot(frame.contentDocument.getElementById('root'));
        let commits = 0, messages = 0;
        const off = store.subscribe(() => ++messages);
        function View() {
            const [count, text] = useStore(store);
            useLayoutEffect(() => { ++commits; });
            return createElement('output', null, `${count} ${text}`);
        }
        try {
            flushSync(() => root.render(createElement(View)));
            const baseline = commits;
            await call(frame, 'reactBurst');
            await until(() => frame.contentDocument.querySelector('output')?.textContent === '999 été 🌍');
            expect(messages).toBe(1);
            expect(commits - baseline).toBe(1);
        } finally {
            root.unmount(); off(); store.dispose(); frame.remove();
        }
    });

    // No React render here (the previous spec counts commits), so each spec stays well within the
    // default timeout even where opening a browser page is slow.
    it('keeps the store of each browser independent and revokes the store of a closed one', async () => {
        const first = await page(), second = await page();
        const store = createStore('counter', [0, 'initial'], {target: first.contentWindow});
        const other = createStore('counter', [0, 'other'], {target: second.contentWindow});
        try {
            const firstId = await call(first, 'reactBurst');
            await until(() => store.getSnapshot()[0] === 999);
            expect(other.getSnapshot()).toEqual([0, 'other']);
            await call(second, 'reactBurst');
            await until(() => other.getSnapshot()[0] === 999);
            expect(other.getSnapshot()).toEqual([999, 'été 🌍']);
            first.remove();
            await until(async () => !await call(second, 'reactStoreValid', firstId));
        } finally {
            store.dispose(); other.dispose(); first.remove(); second.remove();
        }
    });

    it('preserves C++ children through StrictMode effect replay and parent renders; revokes on real unmount', async () => {
        const frame = await page(), dom = frame.contentWindow.dom;
        const root = createRoot(frame.contentDocument.getElementById('root'));
        let mounts = 0, cleanups = 0, unmounted = false;
        function View({version}) {
            useLayoutEffect(() => { ++mounts; return () => ++cleanups; }, []);
            return createElement('section', {'data-version': version}, createElement(Island, {dom, name: 'panel'}));
        }
        const render = version => flushSync(() => root.render(createElement(StrictMode, null, createElement(View, {version}))));
        try {
            render(0);
            await Promise.resolve();
            // This assertion proves development StrictMode actually replayed the effects.
            expect(mounts).toBe(2);
            expect(cleanups).toBe(1);
            await dom.connect();
            const id = await call(frame, 'reactPanel');
            expect(await call(frame, 'domDocumentNodeValid', id)).toBeTrue();
            await until(() => frame.contentDocument.querySelector('section div')?.textContent === 'C++ island');
            const text = frame.contentDocument.querySelector('section div').firstChild;
            render(1);
            expect(frame.contentDocument.querySelector('section div').firstChild).toBe(text);
            root.unmount();
            unmounted = true;
            // Observe the public C++ handle becoming invalid after the page returns its loan.
            await until(async () => !await call(frame, 'domDocumentNodeValid', id));
            const freshRoot = createRoot(frame.contentDocument.getElementById('root'));
            try {
                flushSync(() => freshRoot.render(createElement(Island, {dom, name: 'panel'})));
                await call(frame, 'reactPanel');
                await until(() => frame.contentDocument.getElementById('root').textContent === 'C++ island');
            } finally { freshRoot.unmount(); await Promise.resolve(); }
        } finally {
            if (!unmounted) root.unmount();
            frame.remove();
        }
    });

    async function exampleFrame(blockMount = false) {
        const frame = document.createElement('iframe');
        const loaded = new Promise((resolve, reject) => { frame.onload = resolve; frame.onerror = reject; });
        frame.src = '/react-example.html' + (blockMount ? '?block-mount' : '');
        document.body.appendChild(frame);
        await loaded;
        return frame;
    }

    it('ignores canvas clicks until the maintained example has acknowledged C++ startup', async () => {
        const frame = await exampleFrame();
        try {
            const doc = frame.contentDocument, page = frame.contentWindow;
            await until(() => page.finishStart && doc.querySelector('canvas'));
            const canvas = doc.querySelector('canvas');
            expect(canvas.getAttribute('aria-disabled')).toBe('true');
            const click = () => {
                const rect = canvas.getBoundingClientRect();
                canvas.dispatchEvent(new page.MouseEvent('click', {bubbles: true, clientX: rect.left + 6, clientY: rect.top + 6}));
            };
            click();
            expect(page.toggleCalls).toBe(0);
            const earlyError = await call(frame, 'life.react.toggle', 0).then(() => '', error => String(error.message));
            expect(earlyError).toContain('Life React has not started');
            page.finishStart();
            await until(() => canvas.getAttribute('aria-disabled') === 'false');
            click();
            await until(() => doc.querySelector('[data-testid="react-status"]').textContent.includes('6 living cells'));
            // One direct pre-start probe above, then exactly one accepted UI click.
            expect(page.toggleCalls).toBe(2);
            expect(doc.querySelector('[role="alert"]').textContent).toBe('');
        } finally { frame.remove(); }
    });

    it('reports a bounded startup failure when the maintained example cannot observe its island', async () => {
        const frame = await exampleFrame(true);
        try {
            const doc = frame.contentDocument;
            await until(() => !!doc.querySelector('canvas'));
            // Startup itself has a five-second limit, independently of this test's watchdog.
            const deadline = performance.now() + 8000;
            while (!doc.body.textContent.includes('Life panel did not mount within 5 seconds')) {
                if (performance.now() >= deadline) throw new Error('Life startup did not fail within its bound');
                await new Promise(resolve => setTimeout(resolve, 20));
            }
            expect(frame.contentWindow.finishStart).toBeUndefined();
            expect(Object.hasOwn(frame.contentWindow.webfrontReactStores, 'life')).toBeFalse();
        } finally { frame.remove(); }
    }, 10000);

    it('runs the maintained Game of Life C++ panel inside React and reads its coalesced store', async () => {
        const frame = await page(), dom = frame.contentWindow.dom;
        const store = createStore('life', [new Uint8Array(128), 0, 0], {target: frame.contentWindow});
        const root = createRoot(frame.contentDocument.getElementById('root'));
        function Life() {
            const [cells, generation, population] = useStore(store);
            return createElement('main', null, createElement('output', null, `${generation}:${population}:${cells.length}`),
                createElement(Island, {dom, name: 'life-panel'}));
        }
        try {
            flushSync(() => root.render(createElement(StrictMode, null, createElement(Life))));
            await dom.connect();
            await call(frame, 'life.react.start');
            const doc = frame.contentDocument;
            await until(() => doc.querySelector('output').textContent === '0:5:128' && doc.querySelector('button'));
            const button = doc.querySelector('button');
            button.click();
            await until(() => doc.querySelector('output').textContent === '1:5:128');
            expect(doc.querySelector('p').textContent).toBe('Generation 1 · 5 living cells');
            expect(doc.querySelector('button')).toBe(button);
            await call(frame, 'life.react.toggle', 1023);
            await until(() => doc.querySelector('output').textContent === '0:6:128');
        } finally { root.unmount(); store.dispose(); frame.remove(); }
    }, 10000); // Several bounded bridge waits share this scenario's watchdog.
});
