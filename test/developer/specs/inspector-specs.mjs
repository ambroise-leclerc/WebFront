// Specs of the inspectors' host and page (#680, ADR-027 section 8): the container a panel opens in, the choices kept in the
// project, the floating window, detaching and attaching, and the discovery of the watches. The page in a real window, tab
// or frame is checked in test/web/developer-specs.mjs.
import {detachedContainer, containerFor, hostReasons, parsePreferences, formatPreferences, readPreferences, preferencesPath,
    createInspectorHost, defaultInspectorRect, panelTitle} from '../../../js/developer/environment/inspector-host.mjs';
import {readDescriptor, readPage, listWatches, reconcile, createWatchDiscovery, renderWatches, pageBridge, attachAgain,
    watchesFunction} from '../../../js/developer/environment/inspector.mjs';
import {inspectorChannel, inspectorPanels} from '../../../js/developer/environment/menu.mjs';
import {memoryBridge} from '../memory-bridge.mjs';

const encode = text => new TextEncoder().encode(text);

/** A registry of watches in memory, answering webfront.inspector.watches as C++ does (pages of `size`, revision, changed). */
function memoryWatches(size = 200) {
    const watches = new Map();
    let revision = 1, nextId = 1;
    const calls = [];
    return {
        calls,
        add(name, kind = 'integer') {
            const id = nextId++;
            watches.set(id, [id, name, kind, `app.cpp:${id}`, false]);
            ++revision;
            return id;
        },
        remove(id) {
            if (watches.delete(id)) ++revision;
        },
        answer(known, after, count) {
            calls.push([known, after, count]);
            const served = [...watches.values()].filter(([id]) => id > after).slice(0, Math.min(count, size));
            const more = [...watches.values()].filter(([id]) => id > after).length > served.length;
            return [served, more && count ? served.at(-1)[0] : 0, revision, known !== revision];
        },
    };
}

describe('The inspectors', () => {
    describe('containers', () => {
        it('detach a panel to a native window, else to a tab of the system browser, else nowhere', () => {
            const containers = (secondaryWindow, host, newConnections) => ({secondaryWindow, host, newConnections});
            expect(detachedContainer(containers(true, true, false), {embedded: true})).toBe('window');
            expect(detachedContainer(containers(true, true, true), {embedded: false})).toBe('window');
            // A tab only for a page of the system browser, and only when new loopback connections are admitted.
            expect(detachedContainer(containers(false, true, true), {embedded: false})).toBe('tab');
            expect(detachedContainer(containers(false, true, true), {embedded: true})).toBeNull();
            expect(detachedContainer(containers(false, true, false), {embedded: false})).toBeNull();
            // Nothing for a page that is not admitted, nor without an answer.
            expect(detachedContainer(containers(false, false, true), {embedded: false})).toBeNull();
            expect(detachedContainer(null)).toBeNull();
        });

        it('open a panel detached only when the project keeps it so and a detached container exists', () => {
            const admitted = {secondaryWindow: false, host: true, newConnections: false};
            const detached = new Set(['cpp.watches']);
            expect(containerFor('cpp.watches', {containers: admitted, embedded: true, detached})).toBe('floating');
            expect(containerFor('cpp.watches', {containers: {...admitted, secondaryWindow: true}, embedded: true, detached})).toBe('window');
            expect(containerFor('cpp.functions', {containers: {...admitted, secondaryWindow: true}, embedded: true, detached})).toBe('floating');
            expect(containerFor('cpp.watches', {containers: {...admitted, newConnections: true}, embedded: false, detached})).toBe('tab');
            expect(containerFor('cpp.watches', {containers: null, embedded: true, detached})).toBeNull();
        });
    });

    describe('remembered choices', () => {
        it('keep the detached panels and the floating window\'s place, and ignore what they do not know', () => {
            const text = formatPreferences({detached: new Set(['cpp.watches', 'typescript.bridge']), rect: {x: 1, y: 2, width: 300, height: 200}});
            expect(JSON.parse(text)).toEqual({format: 1, detached: ['cpp.watches', 'typescript.bridge'], rect: {x: 1, y: 2, width: 300, height: 200}});
            const read = parsePreferences(text);
            expect([...read.detached]).toEqual(['cpp.watches', 'typescript.bridge']);
            expect(read.rect).toEqual({x: 1, y: 2, width: 300, height: 200});
            const odd = parsePreferences(JSON.stringify({format: 1, detached: ['nothing', 'cpp.browsers'], rect: {x: 0, y: 0, width: -1, height: 2}}));
            expect([...odd.detached]).toEqual(['cpp.browsers']);
            expect(odd.rect).toBeNull();
            for (const bad of ['', 'nope', JSON.stringify({format: 2, detached: ['cpp.watches']}), JSON.stringify({format: 1, detached: 'cpp.watches'})])
                expect([...parsePreferences(bad).detached]).toEqual([]);
        });

        it('are read from the project, and empty without a file', async () => {
            const {bridge} = memoryBridge(new Map([[preferencesPath, encode(formatPreferences({detached: new Set(['cpp.watches']), rect: null}))]]));
            expect([...(await readPreferences(bridge)).detached]).toEqual(['cpp.watches']);
            expect([...(await readPreferences(memoryBridge().bridge)).detached]).toEqual([]);
        });
    });

    describe('host', () => {
        const hosts = [];
        // A host of this page on a project in memory, whose inspector functions answer as `answers` says.
        async function testHost({containers, embedded = true, files = new Map(), openWindow = async () => {}, windowOpen = () => ({})} = {}) {
            const {bridge: projectBridge, storage} = memoryBridge(files);
            const calls = [];
            const bridge = {cppFunction: name => async (...args) => {
                calls.push([name, ...args]);
                if (name === 'webfront.inspector.openWindow') return openWindow(...args);
                if (name === 'webfront.inspector.embedded') return embedded;
                return projectBridge.cppFunction(name)(...args);
            }};
            const opened = [];
            const host = await createInspectorHost({bridge, page: document, inspectorUrl: '/webfront/developer/environment/inspector.html',
                access: Promise.resolve({containers, reason: ''}), windowOpen: (url, name) => { opened.push([url, name]); return windowOpen(url, name); }});
            hosts.push(host);
            return {host, calls, opened, storage};
        }
        afterEach(() => {
            for (const host of hosts.splice(0)) {
                host.close();
                host.channel?.close();
                host.panel.remove();
            }
            document.head.querySelectorAll('style').forEach(style => { if (style.textContent.includes('.webfront-inspector-window')) style.remove(); });
            delete window.webfrontInspectorHost;
        });
        const admitted = (secondaryWindow, newConnections = false) => ({secondaryWindow, host: true, newConnections});
        const frameUrl = host => new URL(host.frame.getAttribute('src'), location.href);

        it('shows a panel in a floating window whose frame borrows this page\'s bridge', async () => {
            const {host} = await testHost({containers: admitted(false)});
            expect(await host.open('cpp.watches')).toBe('floating');
            expect(host.panel.hidden).toBeFalse();
            expect(host.panel.getAttribute('role')).toBe('dialog');
            expect(host.bar.textContent).toContain(panelTitle('cpp.watches'));
            expect(frameUrl(host).pathname).toBe('/webfront/developer/environment/inspector.html');
            expect(frameUrl(host).searchParams.get('panel')).toBe('cpp.watches');
            expect(frameUrl(host).searchParams.get('frame')).toBe('1');
            expect(window.webfrontInspectorHost).toBe(host);
            // Inside the viewport, at its default place.
            const rect = host.panel.getBoundingClientRect();
            expect(rect.left).toBeGreaterThanOrEqual(0);
            expect(rect.right).toBeLessThanOrEqual(document.documentElement.clientWidth + 1);
            // Another panel replaces the shown one; closing hides the window and unloads its frame.
            await host.open('cpp.functions');
            expect(frameUrl(host).searchParams.get('panel')).toBe('cpp.functions');
            host.closeButton.click();
            expect(host.panel.hidden).toBeTrue();
            expect(host.frame.hasAttribute('src')).toBeFalse();
        });

        it('greys Detach with the reason when no detached container would reach the inspectors', async () => {
            const {host, calls, opened} = await testHost({containers: admitted(false, true), embedded: true});
            await host.open('cpp.watches');
            expect(host.detachButton.getAttribute('aria-disabled')).toBe('true');
            expect(host.detachButton.title).toBe(hostReasons.noDetached);
            host.detachButton.click();
            await host.saved();
            expect(calls.some(([name]) => name === 'webfront.inspector.openWindow')).toBeFalse();
            expect(opened).toEqual([]);
            expect(host.panel.hidden).toBeFalse();
        });

        it('detaches a panel to a native secondary window, and opens it there next time', async () => {
            const {host, calls, storage} = await testHost({containers: admitted(true)});
            await host.open('cpp.watches');
            expect(host.detachButton.getAttribute('aria-disabled')).toBe('false');
            expect(await host.detach('cpp.watches')).toBe('window');
            const [, panel, width, height] = calls.find(([name]) => name === 'webfront.inspector.openWindow');
            expect(panel).toBe('cpp.watches');
            expect(width).toBeGreaterThan(0);
            expect(height).toBeGreaterThan(0);
            expect(host.panel.hidden).toBeTrue();
            await host.saved();
            expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached]).toEqual(['cpp.watches']);
            // The project remembers it: the next opening goes to the window again (which C++ brings to the front).
            calls.length = 0;
            expect(await host.open('cpp.watches')).toBe('window');
            expect(calls.filter(([name]) => name === 'webfront.inspector.openWindow').length).toBe(1);
        });

        it('keeps a panel in the floating window when its native window fails, and says why', async () => {
            const {host} = await testHost({containers: admitted(true), openWindow: async () => { throw new Error('no session'); }});
            expect(await host.detach('cpp.watches')).toBe('floating');
            expect(host.panel.hidden).toBeFalse();
            expect(host.notice.hidden).toBeFalse();
            expect(host.notice.textContent).toContain('no session');
        });

        it('detaches a panel to a tab of the system browser, and falls back to the floating window when it is blocked', async () => {
            let blocked = true;
            const {host, opened} = await testHost({containers: admitted(false, true), embedded: false, windowOpen: () => blocked ? null : {}});
            expect(await host.detach('cpp.watches')).toBe('floating');
            expect(opened.length).toBe(1);
            const [url, name] = opened[0];
            expect(new URL(url).searchParams.get('panel')).toBe('cpp.watches');
            expect(new URL(url).searchParams.has('frame')).toBeFalse();
            expect(name).toBe('webfront-inspector-cpp-watches');
            expect(host.panel.hidden).toBeFalse();
            expect(host.notice.textContent).toContain(hostReasons.blocked);
            const retry = host.notice.querySelector('button');
            expect(retry.hidden).toBeFalse();
            blocked = false;
            retry.click();
            await new Promise(resolve => setTimeout(resolve));
            expect(opened.length).toBe(2);
            expect(host.panel.hidden).toBeTrue();
        });

        it('never opens a tab from an embedded view', async () => {
            const {host, opened} = await testHost({containers: admitted(false, true), embedded: true});
            expect(await host.detach('cpp.watches')).toBeNull();
            expect(opened).toEqual([]);
        });

        it('opens nothing for a page that is not admitted', async () => {
            const {host} = await testHost({containers: null});
            expect(await host.open('cpp.watches')).toBeNull();
            expect(host.panel.hidden).toBeTrue();
        });

        it('attaches a detached panel again when its window asks, and answers it', async () => {
            const {host, storage} = await testHost({containers: admitted(true), files: new Map([[preferencesPath,
                encode(formatPreferences({detached: new Set(['cpp.watches']), rect: null}))]])});
            const inspector = new BroadcastChannel(inspectorChannel);
            try {
                const answered = new Promise(resolve => inspector.addEventListener('message', ({data}) => {
                    if (data?.type === 'attached') resolve(data.id);
                }));
                inspector.postMessage({type: 'attach', panel: 'cpp.watches', id: 'request-1'});
                expect(await answered).toBe('request-1');
                expect(host.shownPanel).toBe('cpp.watches');
                expect(host.panel.hidden).toBeFalse();
                await host.saved();
                expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached]).toEqual([]);
            } finally { inspector.close(); }
        });

        it('opens at a default place inside a small viewport', () => {
            const rect = defaultInspectorRect({width: 400, height: 300});
            expect(rect.x + rect.width).toBeLessThanOrEqual(400);
            expect(rect.y + rect.height).toBeLessThanOrEqual(300);
        });
    });

    describe('page', () => {
        it('borrows the bridge of its host page in a frame, without loading WebFront.js', async () => {
            const bridge = {ready: Promise.resolve()};
            const view = {parent: {webFront: bridge}, document};
            expect(await pageBridge(view, {frame: true})).toBe(bridge);
            await expectAsync(pageBridge({parent: {}, document}, {frame: true})).toBeRejectedWithError(/borrow/);
        });

        it('asks the main page to attach it again, and stays open when nobody answers', async () => {
            const status = document.createElement('p');
            const closed = [];
            const view = {BroadcastChannel, setTimeout: (run, delay) => setTimeout(run, delay), clearTimeout: id => clearTimeout(id), opener: {}, close: () => closed.push(true)};
            const host = new BroadcastChannel(inspectorChannel);
            host.addEventListener('message', ({data}) => {
                if (data?.type === 'attach') host.postMessage({type: 'attached', id: data.id});
            });
            try { expect(await attachAgain(view, null, 'cpp.watches', status)).toBeTrue(); }
            finally { host.close(); }
            expect(closed).toEqual([true]);
            expect(await attachAgain(view, null, 'cpp.watches', status, 50)).toBeFalse();
            expect(status.textContent).toContain('did not answer');
            expect(closed.length).toBe(1);
        });

        it('closes its native window through C++ when it has no opener', async () => {
            const status = document.createElement('p');
            const calls = [];
            const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return true; }};
            const view = {BroadcastChannel, setTimeout: (run, delay) => setTimeout(run, delay), clearTimeout: id => clearTimeout(id), opener: null, close: () => {}};
            const host = new BroadcastChannel(inspectorChannel);
            host.addEventListener('message', ({data}) => host.postMessage({type: 'attached', id: data.id}));
            try { expect(await attachAgain(view, bridge, 'cpp.functions', status)).toBeTrue(); }
            finally { host.close(); }
            expect(calls).toEqual([['webfront.inspector.closeWindow', 'cpp.functions']]);
        });
    });

    describe('watches', () => {
        const bridgeOf = registry => ({cppFunction: name => async (...args) => {
            expect(name).toBe(watchesFunction);
            return registry.answer(...args);
        }});

        it('read the descriptors and pages that C++ sends, and refuse anything else', () => {
            expect(readDescriptor([3, 'grid', 'sequence', 'life.cpp:12', true])).toEqual({id: 3, name: 'grid', kind: 'sequence', location: 'life.cpp:12', writable: true});
            expect(readDescriptor([0, 'grid', 'sequence', 'life.cpp:12', true])).toBeNull();
            expect(readDescriptor([1, 'grid', 'sequence', 'life.cpp:12'])).toBeNull();
            expect(readPage([[], 0, 4, true])).toEqual({descriptors: [], next: 0, revision: 4, changed: true});
            expect(() => readPage([[[1, 'a']], 0, 4, true])).toThrowError(/does not read/);
            expect(() => readPage([[], 0, 4])).toThrowError(/does not read/);
        });

        it('are listed page after page, by identifier', async () => {
            const registry = memoryWatches(2);
            ['a', 'b', 'c', 'd', 'e'].forEach(name => registry.add(name));
            const {watches, revision} = await listWatches(bridgeOf(registry), 0);
            expect(watches.map(watch => watch.name)).toEqual(['a', 'b', 'c', 'd', 'e']);
            expect(revision).toBe(6);
            expect(registry.calls.map(([, after]) => after)).toEqual([0, 2, 4]);
        });

        it('reconcile the watches shown with a new list', () => {
            const known = new Map([[1, {id: 1}], [2, {id: 2}]]);
            expect(reconcile(known, [{id: 2}, {id: 3}])).toEqual({added: [{id: 3}], removed: [1]});
        });

        it('follow registrations and removals, asking only for the revision while nothing changes', async () => {
            jasmine.clock().install();
            try {
                const registry = memoryWatches();
                const grid = registry.add('grid', 'sequence');
                const changes = [];
                const discovery = createWatchDiscovery({bridge: bridgeOf(registry), onChange: change => changes.push(change), period: 250});
                await discovery.start();
                expect(discovery.watches.map(watch => watch.name)).toEqual(['grid']);
                registry.calls.length = 0;
                // Nothing changed: one call with a count of 0 per period.
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(registry.calls).toEqual([[2, 0, 0]]);
                // A registration appears, a removal disappears.
                const generations = registry.add('generations');
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(discovery.watches.map(watch => watch.name)).toEqual(['grid', 'generations']);
                registry.remove(grid);
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(discovery.watches.map(watch => watch.id)).toEqual([generations]);
                expect(changes.at(-1).removed).toEqual([grid]);
                // A call that finds a watch gone forgets it at once; paused, nothing is asked.
                discovery.forget(generations);
                expect(discovery.watches).toEqual([]);
                discovery.pause();
                registry.calls.length = 0;
                jasmine.clock().tick(1000);
                expect(registry.calls).toEqual([]);
                discovery.resume();
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(registry.calls.length).toBeGreaterThan(0);
                discovery.stop();
            } finally { jasmine.clock().uninstall(); }
        });

        it('keep trying after a refused read', async () => {
            jasmine.clock().install();
            try {
                const registry = memoryWatches();
                let refuse = true;
                const errors = [];
                const bridge = {cppFunction: () => async (...args) => {
                    if (refuse) throw Object.assign(new Error('denied'), {code: 'webfront.inspector.denied'});
                    return registry.answer(...args);
                }};
                registry.add('grid');
                const discovery = createWatchDiscovery({bridge, onChange: () => {}, onError: error => errors.push(error.code), period: 250});
                await discovery.start();
                expect(errors).toEqual(['webfront.inspector.denied']);
                refuse = false;
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(discovery.watches.map(watch => watch.name)).toEqual(['grid']);
                discovery.stop();
            } finally { jasmine.clock().uninstall(); }
        });

        it('render one item per watch, and remove the item of a watch that disappeared', () => {
            const list = document.createElement('ul');
            renderWatches(list, [{id: 1, name: 'grid', kind: 'sequence', location: 'life.cpp:12', writable: false},
                {id: 2, name: 'speed', kind: 'integer', location: 'life.cpp:20', writable: true}]);
            expect([...list.children].map(item => item.dataset.watch)).toEqual(['1', '2']);
            expect(list.children[1].textContent).toContain('writable');
            const first = list.children[0];
            renderWatches(list, [{id: 1, name: 'grid', kind: 'sequence', location: 'life.cpp:12', writable: false}]);
            expect([...list.children]).toEqual([first]);
        });

        it('name every panel of the menu', () => {
            expect([...inspectorPanels.keys()]).toEqual(['cpp.functions', 'cpp.browsers', 'cpp.watches', 'typescript.packages', 'typescript.bridge']);
        });
    });
});
