// Specs of the inspectors' host and page (#680, ADR-027 section 8): the container a panel opens in, the choices kept in the
// project, the floating window, detaching and attaching, and the discovery of the watches. The page in a real window, tab
// or frame is checked in test/web/developer-specs.mjs.
import {detachedContainer, containerFor, hostReasons, parsePreferences, formatPreferences, readPreferences, preferencesPath,
    createInspectorHost, defaultInspectorRect, panelTitle, claimHostIdentifier, hostKey, prepareInspectors,
    preferencesStore, applyPreferenceChanges} from '../../../js/developer/environment/inspector-host.mjs';
import {readDescriptor, readPage, listWatches, listingAttempts, reconcile, watchOpener, createWatchDiscovery, renderWatches, pageBridge, attachAgain,
    watchesFunction, reloadWhenRestored} from '../../../js/developer/environment/inspector.mjs';
import {inspectorChannel, inspectorPanels, inspectorMoves, makeMoveButton} from '../../../js/developer/environment/menu.mjs';
import {LineEditor} from '../../../js/developer/environment/terminal.mjs';
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

    describe('choices kept by several pages', () => {
        it('apply the changes of a page to the choices of the project, not its whole copy', () => {
            const current = {detached: new Set(['cpp.watches']), rect: {x: 1, y: 2, width: 300, height: 200}};
            const next = applyPreferenceChanges(current, {panels: new Map([['cpp.functions', true]]), rect: undefined});
            expect([...next.detached].sort()).toEqual(['cpp.functions', 'cpp.watches']);
            expect(next.rect).toEqual(current.rect);
            expect([...applyPreferenceChanges(current, {panels: new Map([['cpp.watches', false]]), rect: null}).detached]).toEqual([]);
        });

        it('keep what another page wrote meanwhile, and write a detach at once', async () => {
            jasmine.clock().install();
            try {
                const {bridge, storage} = memoryBridge();
                const first = preferencesStore(bridge), second = preferencesStore(bridge);
                // The first page detaches Watches: written at once, without waiting for the delay of the places.
                first.detached('cpp.watches', true);
                await first.saved();
                expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached]).toEqual(['cpp.watches']);
                // The second page, which read the file before, moves its window: the detach of the first is kept.
                second.place({x: 5, y: 6, width: 400, height: 300});
                jasmine.clock().tick(300);
                await second.saved();
                const kept = parsePreferences(new TextDecoder().decode(storage.get(preferencesPath)));
                expect([...kept.detached]).toEqual(['cpp.watches']);
                expect(kept.rect).toEqual({x: 5, y: 6, width: 400, height: 300});
            } finally { jasmine.clock().uninstall(); }
        });

        it('rejects a failed save and can save again from a later action', async () => {
            const {bridge: memory, storage} = memoryBridge();
            const calls = [];
            let connected = false;
            const bridge = {cppFunction: name => async (...args) => {
                calls.push(name);
                if (name === 'webfront.inspector.savePreferences' && !connected) throw new Error('Disconnected');
                return memory.cppFunction(name)(...args);
            }};
            const store = preferencesStore(bridge);
            store.detached('cpp.watches', true);
            await expectAsync(store.saved()).toBeRejectedWithError('Disconnected');
            expect(storage.has(preferencesPath)).toBeFalse();
            // One read gives the file (absent here) and the token of exactly what it read.
            expect(calls).toEqual(['webfront.inspector.readPreferences', 'webfront.inspector.savePreferences']);
            expect(storage.has(preferencesPath)).toBeFalse();
            connected = true;
            store.detached('cpp.watches', true);
            await store.saved();
            expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached]).toEqual(['cpp.watches']);
        });

        it('do not commit over choices that could not be read', async () => {
            const others = encode(formatPreferences({detached: new Set(['cpp.functions']), rect: {x: 1, y: 2, width: 300, height: 200}}));
            const {bridge: memory, storage} = memoryBridge(new Map([[preferencesPath, others]]));
            let failing = true;
            const bridge = {cppFunction: name => async (...args) => {
                if (name === 'webfront.inspector.readPreferences' && failing) throw new Error('Cannot read project file: ' + preferencesPath);
                return memory.cppFunction(name)(...args);
            }};
            const store = preferencesStore(bridge);
            store.detached('cpp.watches', true);
            await expectAsync(store.saved()).toBeRejectedWithError(/Cannot read project file/);
            expect(storage.get(preferencesPath)).toBe(others);
            // Shown, they are empty: the inspectors still open.
            expect([...(await readPreferences(bridge)).detached]).toEqual([]);
            failing = false;
            store.detached('cpp.watches', true);
            await store.saved();
            expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached].sort())
                .toEqual(['cpp.functions', 'cpp.watches']);
        });

        it('keep a change whose save failed, and write it with the next one without asking for it again', async () => {
            const {bridge: memory, storage} = memoryBridge();
            let connected = false;
            const bridge = {cppFunction: name => async (...args) => {
                if (name === 'webfront.inspector.savePreferences' && !connected) throw new Error('Disconnected');
                return memory.cppFunction(name)(...args);
            }};
            const store = preferencesStore(bridge);
            store.detached('cpp.watches', true);
            await expectAsync(store.saved()).toBeRejectedWithError('Disconnected');
            connected = true;
            // Only a move after the reconnection: the detach that failed is written with it.
            store.place({x: 5, y: 6, width: 400, height: 300});
            await store.saved();
            const written = parsePreferences(new TextDecoder().decode(storage.get(preferencesPath)));
            expect([...written.detached]).toEqual(['cpp.watches']);
            expect(written.rect).toEqual({x: 5, y: 6, width: 400, height: 300});
        });

        it('let a newer change of a panel win over the failed one it replaces', async () => {
            const {bridge: memory, storage} = memoryBridge();
            let connected = false;
            const bridge = {cppFunction: name => async (...args) => {
                if (name === 'webfront.inspector.savePreferences' && !connected) throw new Error('Disconnected');
                return memory.cppFunction(name)(...args);
            }};
            const store = preferencesStore(bridge);
            store.detached('cpp.watches', true);
            await expectAsync(store.saved()).toBeRejectedWithError('Disconnected');
            connected = true;
            store.detached('cpp.watches', false);  // attached again since: the newer change is the one written
            await store.saved();
            expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached]).toEqual([]);
        });

        it('fall back to the token then the file with a library that cannot read them together', async () => {
            const {bridge: memory, storage} = memoryBridge(new Map([[preferencesPath, encode(formatPreferences({detached: new Set(['cpp.functions']), rect: null}))]]));
            const calls = [];
            const bridge = {cppFunction: name => async (...args) => {
                calls.push(name);
                if (name === 'webfront.inspector.readPreferences')
                    throw Object.assign(new Error("C++ function 'webfront.inspector.readPreferences' was not found"), {code: 'webfront.notFound'});
                return memory.cppFunction(name)(...args);
            }};
            const store = preferencesStore(bridge);
            store.detached('cpp.watches', true);
            await store.saved();
            expect(calls.slice(0, 3)).toEqual(['webfront.inspector.readPreferences', 'webfront.developer.state', 'webfront.developer.read']);
            expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached].sort()).toEqual(['cpp.functions', 'cpp.watches']);
        });

        it('write again a change whose file changed between its read and its commit', async () => {
            const {bridge: memory, storage} = memoryBridge();
            let raced = false;
            const bridge = {cppFunction: name => async (...args) => {
                // Another page commits between this page's read and its commit, once.
                if (name === 'webfront.inspector.savePreferences' && !raced) {
                    raced = true;
                    storage.set(preferencesPath, encode(formatPreferences({detached: new Set(['typescript.bridge']), rect: null})));
                }
                return memory.cppFunction(name)(...args);
            }};
            const store = preferencesStore(bridge);
            store.detached('cpp.watches', true);
            await store.saved();
            expect(raced).toBeTrue();
            expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached].sort())
                .toEqual(['cpp.watches', 'typescript.bridge']);
        });
    });

    describe('host', () => {
        const hosts = [];
        // A host of this page on a project in memory, whose inspector functions answer as `answers` says.
        async function testHost({containers, embedded = true, files = new Map(), openWindow = async () => {}, closeWindow = async () => true,
            windowOpen = () => ({}), hostId = 'host-a'} = {}) {
            const {bridge: projectBridge, storage} = memoryBridge(files);
            const calls = [];
            const bridge = {cppFunction: name => async (...args) => {
                calls.push([name, ...args]);
                if (name === 'webfront.inspector.openWindow') return openWindow(...args);
                if (name === 'webfront.inspector.closeWindow') return closeWindow(...args);
                if (name === 'webfront.inspector.embedded') return embedded;
                return projectBridge.cppFunction(name)(...args);
            }};
            const opened = [];
            const host = await createInspectorHost({bridge, page: document, inspectorUrl: '/webfront/developer/environment/inspector.html',
                access: Promise.resolve({containers, reason: ''}), hostId, windowOpen: (url, name) => { opened.push([url, name]); return windowOpen(url, name); }});
            hosts.push(host);
            return {host, bridge, calls, opened, storage};
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
            const {host, bridge} = await testHost({containers: admitted(false)});
            expect(await host.open('cpp.watches')).toBe('floating');
            expect(host.panel.hidden).toBeFalse();
            expect(host.panel.getAttribute('role')).toBe('dialog');
            expect(host.bar.textContent).toContain(panelTitle('cpp.watches'));
            expect(frameUrl(host).pathname).toBe('/webfront/developer/environment/inspector.html');
            expect(frameUrl(host).searchParams.get('panel')).toBe('cpp.watches');
            expect(frameUrl(host).searchParams.get('frame')).toBe('1');
            expect(window.webfrontInspectorHost).toBe(host);
            expect(host.bridge).toBe(bridge);  // the bridge its frame borrows
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

        it('draws Detach and Attach as icons named by their action (#722)', () => {
            for (const move of ['detach', 'attach']) {
                const button = makeMoveButton(document, document.createElement('button'), move);
                expect(button.textContent).toBe('');
                expect(button.getAttribute('aria-label')).toBe(inspectorMoves[move]);
                expect(button.title).toBe(inspectorMoves[move]);
                const svg = button.firstElementChild;
                expect(button.children.length).toBe(1);
                expect(svg.namespaceURI).toBe('http://www.w3.org/2000/svg');
                expect(svg.getAttribute('stroke')).toBe('currentColor');
                expect(svg.getAttribute('aria-hidden')).toBe('true');
                expect(svg.querySelectorAll('path').length).toBeGreaterThan(1);
            }
            expect(inspectorMoves.detach).toBe('Detach this panel to a window of its own');
            expect(inspectorMoves.attach).toBe('Show this panel in the floating window of the main page');
        });

        it('names the Detach icon by its action, and its tooltip too when it can detach', async () => {
            const {host} = await testHost({containers: admitted(true)});
            await host.open('cpp.watches');
            expect(host.detachButton.textContent).toBe('');
            expect(host.detachButton.querySelector('svg')).not.toBeNull();
            expect(host.detachButton.getAttribute('aria-label')).toBe(inspectorMoves.detach);
            expect(host.detachButton.title).toBe(inspectorMoves.detach);
            expect(host.closeButton.getAttribute('aria-label')).toBe('Close the inspector');
        });

        it('greys Detach with the reason when no detached container would reach the inspectors', async () => {
            const {host, calls, opened} = await testHost({containers: admitted(false, true), embedded: true});
            await host.open('cpp.watches');
            expect(host.detachButton.getAttribute('aria-disabled')).toBe('true');
            expect(host.detachButton.title).toBe(hostReasons.noDetached);
            expect(host.detachButton.getAttribute('aria-label')).toBe(inspectorMoves.detach);
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
            const [, panel, hostId, width, height] = calls.find(([name]) => name === 'webfront.inspector.openWindow');
            expect(panel).toBe('cpp.watches');
            expect(hostId).toBe(host.id);
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
            expect(new URL(url).searchParams.get('host')).toBe('host-a');
            expect(name).toBe('webfront-inspector-host-a-cpp-watches');
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

        it('opens a tab of its own for each page, even for the same panel', async () => {
            const {host: firstHost, opened: first} = await testHost({containers: admitted(false, true), embedded: false, hostId: 'host-a'});
            const {host: second, opened: other} = await testHost({containers: admitted(false, true), embedded: false, hostId: 'host-b'});
            await firstHost.detach('cpp.watches');
            await second.detach('cpp.watches');
            expect(first[0][1]).not.toBe(other[0][1]);
            expect(new URL(other[0][0]).searchParams.get('host')).toBe('host-b');
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
                inspector.postMessage({type: 'attach', panel: 'cpp.watches', host: 'host-a', id: 'request-1'});
                expect(await answered).toBe('request-1');
                expect(host.shownPanel).toBe('cpp.watches');
                expect(host.panel.hidden).toBeFalse();
                await host.saved();
                expect([...parsePreferences(new TextDecoder().decode(storage.get(preferencesPath))).detached]).toEqual([]);
            } finally { inspector.close(); }
        });

        // Waits for what a message of the channel causes, two seconds at most: the expectation that follows says what is missing.
        async function delivered(done) {
            for (let waited = 0; !done() && waited < 2000; waited += 5) await new Promise(resolve => setTimeout(resolve, 5));
        }

        it('attaches a panel again only in the page that detached it', async () => {
            const {host: mine} = await testHost({containers: admitted(true), hostId: 'host-a'});
            const {host: other, storage: otherStorage} = await testHost({containers: admitted(true), hostId: 'host-b'});
            const inspector = new BroadcastChannel(inspectorChannel);
            try {
                const answers = [];
                inspector.addEventListener('message', ({data}) => { if (data?.type === 'attached') answers.push(data.id); });
                inspector.postMessage({type: 'attach', panel: 'cpp.watches', host: 'host-a', id: 'request-a'});
                await delivered(() => answers.length > 0);
                await new Promise(resolve => setTimeout(resolve, 50));  // and nobody else answers
                expect(answers).toEqual(['request-a']);
                expect(mine.shownPanel).toBe('cpp.watches');
                expect(other.shownPanel).toBeNull();
                expect(other.panel.hidden).toBeTrue();
                // A request without a host, or for a page that is gone, is taken by nobody.
                inspector.postMessage({type: 'attach', panel: 'cpp.functions', id: 'request-none'});
                inspector.postMessage({type: 'attach', panel: 'cpp.functions', host: 'host-gone', id: 'request-gone'});
                await new Promise(resolve => setTimeout(resolve, 50));
                expect(answers).toEqual(['request-a']);
                expect(mine.shownPanel).toBe('cpp.watches');
                await other.saved();
                expect(otherStorage.has(preferencesPath)).toBeFalse();
            } finally { inspector.close(); }
        });

        // The window that asked to be attached again, on the channel: resolves with the host's answer.
        async function askAttach(panel, request) {
            const inspector = new BroadcastChannel(inspectorChannel);
            try {
                return await new Promise(resolve => {
                    inspector.addEventListener('message', ({data}) => { if (data?.type === 'attached' && data.id === request) resolve(data); });
                    inspector.postMessage({type: 'attach', panel, host: 'host-a', id: request});
                });
            } finally { inspector.close(); }
        }

        it('closes the native window of a panel attached again itself, before a new detach opens one', async () => {
            let closed;
            const {host, calls} = await testHost({containers: admitted(true), closeWindow: () => new Promise(resolve => { closed = resolve; })});
            await host.detach('cpp.watches');
            const answer = askAttach('cpp.watches', 'request-close');  // answered once the close is over
            const names = () => calls.map(([name]) => name).filter(name => name.endsWith('Window'));
            // The request crosses the channel in its own time (more than 20 ms on a loaded WebView2 runner): waited for, not timed.
            await delivered(() => names().includes('webfront.inspector.closeWindow'));
            expect(host.preferences.detached.has('cpp.watches')).toBeFalse();
            // The user detaches it again while the close is in progress: the window opens only once the close is over.
            const detaching = host.detach('cpp.watches');
            await new Promise(resolve => setTimeout(resolve));
            expect(names()).toEqual(['webfront.inspector.openWindow', 'webfront.inspector.closeWindow']);
            closed(true);
            expect((await answer).closer).toBe('host');
            expect(await detaching).toBe('window');
            expect(names()).toEqual(['webfront.inspector.openWindow', 'webfront.inspector.closeWindow', 'webfront.inspector.openWindow']);
            expect(host.preferences.detached.has('cpp.watches')).toBeTrue();
        });

        it('lets the native window close itself when the host could not close it', async () => {
            for (const closeWindow of [async () => false, async () => { throw new Error('no session'); }]) {
                const {host} = await testHost({containers: admitted(true), closeWindow});
                await host.detach('cpp.watches');
                // The window waits for the host only when the host closed it: else it would stay open.
                expect((await askAttach('cpp.watches', `request-${hosts.length}`)).closer).toBe('self');
                expect(host.shownPanel).toBe('cpp.watches');
                host.close();
                host.channel?.close();
            }
        });

        it('closes a tab it opened when its panel is attached again, and opens a new one on the next detach', async () => {
            const tabs = [];
            const {host, opened} = await testHost({containers: admitted(false, true), embedded: false, windowOpen: () => {
                const tab = {closed: false, close() { this.closed = true; }};
                tabs.push(tab);
                return tab;
            }});
            await host.detach('cpp.watches');
            expect((await askAttach('cpp.watches', 'request-tab')).closer).toBe('host');
            expect(tabs[0].closed).toBeTrue();
            expect(await host.detach('cpp.watches')).toBe('tab');
            expect(tabs[1].closed).toBeFalse();
            expect(opened.map(([, name]) => name)).toEqual(['webfront-inspector-host-a-cpp-watches', 'webfront-inspector-host-a-cpp-watches']);
        });

        it('lets a tab it did not open close itself, and never reuses that tab for a new detach', async () => {
            // The tab was opened before this page was reloaded: this page has no reference to it.
            const {host, opened} = await testHost({containers: admitted(false, true), embedded: false, files: new Map([[preferencesPath,
                encode(formatPreferences({detached: new Set(['cpp.watches']), rect: null}))]])});
            expect((await askAttach('cpp.watches', 'request-orphan')).closer).toBe('self');
            expect(await host.detach('cpp.watches')).toBe('tab');
            expect(opened.map(([, name]) => name)).toEqual(['webfront-inspector-host-a-cpp-watches-1']);
        });

        // The Web Locks of an origin, shared by the pages of a test: a page holds a lock until it is released.
        function originLocks() {
            const held = new Set();
            return {held, locks: {request: async (name, options, callback) => {
                const available = !held.has(name);
                if (available) held.add(name);
                const result = callback(available ? {name} : null);
                if (available) await result;
                return result;
            }}};
        }
        const pageOf = ({locks, store = new Map()}) => ({crypto, navigator: {locks},
            sessionStorage: {getItem: key => store.get(key) ?? null, setItem: (key, value) => store.set(key, value)}, store});

        it('keeps a page\'s identifier across a reload of its tab', async () => {
            const first = originLocks(), store = new Map();
            const id = await claimHostIdentifier(pageOf({locks: first.locks, store}));
            expect(id).toMatch(/^[A-Za-z0-9_-]{1,64}$/);
            expect(store.get(hostKey)).toBe(id);
            // The reloaded page: the old one released its lock as it went away.
            const reloaded = originLocks();
            expect(await claimHostIdentifier(pageOf({locks: reloaded.locks, store}))).toBe(id);
        });

        it('gives a page opened by another one, whose session storage is a copy, an identifier of its own', async () => {
            const origin = originLocks(), store = new Map();
            const opener = await claimHostIdentifier(pageOf({locks: origin.locks, store}));
            const copied = new Map(store);
            const opened = await claimHostIdentifier(pageOf({locks: origin.locks, store: copied}));
            expect(opened).not.toBe(opener);
            expect(copied.get(hostKey)).toBe(opened);
            expect(store.get(hostKey)).toBe(opener);
            expect(origin.held.size).toBe(2);
        });

        it('takes a new identifier at each load without Web Locks or storage, and never keeps an invalid one', async () => {
            const store = new Map([[hostKey, 'bad/value']]);
            const view = {crypto, sessionStorage: {getItem: key => store.get(key) ?? null, setItem: (key, value) => store.set(key, value)}};
            const first = await claimHostIdentifier(view);
            expect(first).toMatch(/^[0-9a-f]{24}$/);
            expect(await claimHostIdentifier(view)).not.toBe(first);
            const noStorage = {crypto, get sessionStorage() { throw new Error('denied'); }};
            expect(await claimHostIdentifier(noStorage)).toMatch(/^[0-9a-f]{24}$/);
        });

        it('listens for its detached panels as soon as the page reaches the inspectors, before any panel is opened', async () => {
            // The page was reloaded: a window detached before the reload carries its identifier and asks to be attached.
            const {bridge} = memoryBridge();
            const fullBridge = {cppFunction: name => name === 'webfront.inspector.embedded' ? async () => true : bridge.cppFunction(name)};
            const options = {bridge: fullBridge, page: document, inspectorUrl: '/webfront/developer/environment/inspector.html',
                access: Promise.resolve({containers: admitted(true), reason: ''}), hostId: 'reloaded', windowOpen: () => ({})};
            const inspectors = prepareInspectors(options);
            const host = await new Promise(resolve => {
                const wait = () => inspectors.host() ? resolve(inspectors.host()) : setTimeout(wait);
                wait();
            });
            hosts.push(host);
            const inspector = new BroadcastChannel(inspectorChannel);
            try {
                const answered = new Promise(resolve => inspector.addEventListener('message', ({data}) => {
                    if (data?.type === 'attached') resolve(data.id);
                }));
                inspector.postMessage({type: 'attach', panel: 'cpp.watches', host: 'reloaded', id: 'after-reload'});
                expect(await answered).toBe('after-reload');
                expect(host.shownPanel).toBe('cpp.watches');
            } finally { inspector.close(); }
        });

        it('makes no host for a page that does not reach the inspectors until a panel is asked for', async () => {
            const made = [];
            const inspectors = prepareInspectors({access: Promise.resolve({containers: null, reason: 'no'}), create: options => {
                made.push(options);
                return Promise.resolve({open: panel => panel});
            }});
            await new Promise(resolve => setTimeout(resolve));
            expect(inspectors.host()).toBeNull();
            expect(await inspectors.open('cpp.watches')).toBe('cpp.watches');
            expect(made.length).toBe(1);
        });

        it('creates the host again on the next panel after a failed creation', async () => {
            let attempts = 0;
            const inspectors = prepareInspectors({access: Promise.resolve({containers: admitted(true), reason: ''}), create: async () => {
                if (++attempts === 1) throw new Error('no host');
                return {open: panel => panel};
            }});
            await new Promise(resolve => setTimeout(resolve));  // the creation started by the access fails
            expect(attempts).toBe(1);
            expect(inspectors.host()).toBeNull();
            expect(await inspectors.open('cpp.watches')).toBe('cpp.watches');
            expect(attempts).toBe(2);
        });

        it('does not show a panel again when its native window fails after another action', async () => {
            let fail;
            const pending = new Promise((resolve, reject) => { fail = reject; });
            const {host} = await testHost({containers: admitted(true), openWindow: () => pending});
            await host.open('cpp.watches');
            const detaching = host.detach('cpp.watches');
            await host.open('cpp.functions');  // the user opens another panel meanwhile
            fail(new Error('no session'));
            expect(await detaching).toBe('floating');
            expect(host.shownPanel).toBe('cpp.functions');
            expect(host.notice.hidden).toBeTrue();
        });

        it('keeps a panel attached while its detach was pending, and closes the native window that opened late', async () => {
            let opened;
            const pending = new Promise(resolve => { opened = resolve; });
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => pending});
            await host.open('cpp.watches');
            const detaching = host.detach('cpp.watches');
            host.attach('cpp.watches');  // the panel is attached again before C++ answered
            expect(calls.some(([name]) => name === 'webfront.inspector.closeWindow')).toBeFalse();
            opened();  // the native window opens after all
            expect(await detaching).toBe('floating');
            expect(host.panel.hidden).toBeFalse();
            expect(host.shownPanel).toBe('cpp.watches');
            expect(host.preferences.detached.has('cpp.watches')).toBeFalse();
            // Shown once: the stale native window is closed, the floating one stays.
            expect(calls.filter(([name]) => name === 'webfront.inspector.closeWindow')).toEqual([['webfront.inspector.closeWindow', 'cpp.watches', 'host-a']]);
        });

        it('closes the native window that opens after the user closed the floating window', async () => {
            let opened;
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => new Promise(resolve => { opened = resolve; })});
            await host.open('cpp.watches');
            const detaching = host.detach('cpp.watches');
            host.closeButton.click();  // the user closes the inspector before C++ answered
            opened();
            await detaching;
            expect(host.panel.hidden).toBeTrue();
            expect(calls.filter(([name]) => name === 'webfront.inspector.closeWindow')).toEqual([['webfront.inspector.closeWindow', 'cpp.watches', 'host-a']]);
            // The panel stays remembered detached: the next opening goes to its window.
            expect(host.preferences.detached.has('cpp.watches')).toBeTrue();
        });

        it('closes every native window still opening for the panel when the floating window is closed, after Detach clicked twice', async () => {
            const answers = [];
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => new Promise(resolve => answers.push(resolve))});
            await host.open('cpp.watches');
            const first = host.detach('cpp.watches');
            const second = host.detach('cpp.watches');
            expect(answers.length).toBe(2);
            host.closeButton.click();
            answers[0]();  // the first opening answers after the close too
            answers[1]();
            await Promise.all([first, second]);
            expect(host.panel.hidden).toBeTrue();
            // Both aimed at the panel's one native window: the latest opening closes it.
            expect(calls.filter(([name]) => name === 'webfront.inspector.closeWindow')).toEqual([['webfront.inspector.closeWindow', 'cpp.watches', 'host-a']]);
        });

        it('closes the window once every cancelled opening has answered, whatever their order', async () => {
            const answers = [];
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => new Promise(resolve => answers.push(resolve))});
            await host.open('cpp.watches');
            const first = host.detach('cpp.watches');
            const second = host.detach('cpp.watches');
            host.closeButton.click();  // cancels both
            answers[1]();  // the latest opening answers first: the first one may still create the window
            await second;
            expect(calls.some(([name]) => name === 'webfront.inspector.closeWindow')).toBeFalse();
            answers[0]();
            expect(await first).toBe('floating');
            expect(calls.filter(([name]) => name === 'webfront.inspector.closeWindow')).toEqual([['webfront.inspector.closeWindow', 'cpp.watches', 'host-a']]);
            expect(host.panel.hidden).toBeTrue();
        });

        it('does not let a cancelled opening close the window that a later detach of the panel reuses', async () => {
            const answers = [];
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => new Promise(resolve => answers.push(resolve))});
            await host.open('cpp.watches');
            const cancelled = host.detach('cpp.watches');
            host.closeButton.click();  // cancels the first opening
            const again = host.detach('cpp.watches');  // the user detaches it again at once
            expect(answers.length).toBe(2);
            answers[0]();  // the cancelled opening answers first
            await cancelled;
            expect(calls.some(([name]) => name === 'webfront.inspector.closeWindow')).toBeFalse();
            answers[1]();
            expect(await again).toBe('window');
            expect(calls.some(([name]) => name === 'webfront.inspector.closeWindow')).toBeFalse();
            expect(host.panel.hidden).toBeTrue();
        });

        it('keeps the native window when another panel is opened in the floating window meanwhile', async () => {
            let opened;
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => new Promise(resolve => { opened = resolve; })});
            await host.open('cpp.watches');
            const detaching = host.detach('cpp.watches');
            await host.open('cpp.functions');
            host.closeButton.click();  // closes Functions, not the panel being detached
            opened();
            expect(await detaching).toBe('window');
            expect(calls.some(([name]) => name === 'webfront.inspector.closeWindow')).toBeFalse();
        });

        it('keeps the native window of a panel detached again while its first opening was pending', async () => {
            const answers = [];
            const {host, calls} = await testHost({containers: admitted(true), openWindow: () => new Promise(resolve => answers.push(resolve))});
            await host.open('cpp.watches');
            const first = host.detach('cpp.watches');
            host.attach('cpp.watches');
            const second = host.detach('cpp.watches');  // detached again: the window that opens is wanted
            answers.forEach(resolve => resolve());
            expect(await first).toBe('window');
            expect(await second).toBe('window');
            expect(calls.some(([name]) => name === 'webfront.inspector.closeWindow')).toBeFalse();
            expect(host.panel.hidden).toBeTrue();
        });

        it('says in the terminal that a panel could not open, above the line being edited', () => {
            let output = '';
            const editor = new LineEditor({write: text => { output += text; }, run: async () => {}});
            editor.showPrompt();
            editor.handle('ls');
            output = '';
            editor.notice('The inspector could not open: no host');
            // The edited line is erased, the notice printed on its own line, then the prompt and the line drawn again.
            expect(output.startsWith('\r\x1b[JThe inspector could not open: no host\r\n> ')).toBeTrue();
            expect(output).toContain('ls');
        });

        it('draws the line again after a notice received while a completion is pending', async () => {
            let output = '', answer;
            const ran = [];
            const editor = new LineEditor({write: text => { output += text; }, run: async line => { ran.push(line); },
                complete: () => new Promise(resolve => { answer = resolve; })});
            editor.showPrompt();
            editor.handle('ls');
            editor.handle('\t');  // the completion is pending
            output = '';
            editor.notice('The inspector could not open: no host');
            expect(output.startsWith('\r\x1b[JThe inspector could not open: no host\r\n> ')).toBeTrue();
            expect(output).toContain('ls');
            answer({candidates: []});  // no candidate: the completion draws nothing
            await new Promise(resolve => setTimeout(resolve));
            editor.handle('\r');
            await editor.whenIdle();
            expect(ran).toEqual(['ls']);
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
            // The host's bridge, not the page's global one: the environment may be mounted with another bridge.
            const global = {ready: Promise.resolve()};
            const view = {parent: {webFront: global, webfrontInspectorHost: {bridge}}, document};
            expect(await pageBridge(view, {frame: true})).toBe(bridge);
            // Without a host, the global bridge of the page is never borrowed.
            await expectAsync(pageBridge({parent: {webFront: global}, document}, {frame: true})).toBeRejectedWithError(/borrow/);
            await expectAsync(pageBridge({parent: {}, document}, {frame: true})).toBeRejectedWithError(/borrow/);
        });

        it('asks the main page to attach it again, and stays open when nobody answers', async () => {
            const status = document.createElement('p');
            const closed = [];
            const view = {BroadcastChannel, setTimeout: (run, delay) => setTimeout(run, delay), clearTimeout: id => clearTimeout(id), opener: {}, close: () => closed.push(true)};
            const host = new BroadcastChannel(inspectorChannel);
            host.addEventListener('message', ({data}) => {
                if (data?.type === 'attach' && data.host === 'host-a') host.postMessage({type: 'attached', id: data.id});
            });
            try {
                expect(await attachAgain(view, null, 'cpp.watches', status, {host: 'host-b', wait: 50})).toBeFalse();  // another page
                expect(closed).toEqual([]);
                expect(await attachAgain(view, null, 'cpp.watches', status, {host: 'host-a'})).toBeTrue();
            } finally { host.close(); }
            expect(closed).toEqual([true]);
            expect(await attachAgain(view, null, 'cpp.watches', status, {host: 'host-a', wait: 50})).toBeFalse();
            expect(status.textContent).toContain('did not answer');
            expect(closed.length).toBe(1);
        });

        it('closes a tab once the page that opened it is gone, and watches nothing without an opener', () => {
            jasmine.clock().install();
            try {
                const opener = {closed: false}, closed = [];
                const handlers = [];
                const view = {opener, setInterval: (run, delay) => setInterval(run, delay), clearInterval: id => clearInterval(id),
                    close: () => closed.push(true), addEventListener: (name, handler) => handlers.push([name, handler])};
                expect(watchOpener(view, 1000)).not.toBeNull();
                jasmine.clock().tick(1000);
                expect(closed).toEqual([]);
                opener.closed = true;
                jasmine.clock().tick(1000);
                expect(closed).toEqual([true]);
                handlers.find(([name]) => name === 'pagehide')[1]();
                expect(watchOpener({...view, opener: null})).toBeNull();
            } finally { jasmine.clock().uninstall(); }
        });

        it('loads itself again when the browser restores it from its back-forward cache', () => {
            const handlers = [], reloads = [];
            const view = {addEventListener: (name, handler) => handlers.push([name, handler]), location: {reload: () => reloads.push(true)}};
            reloadWhenRestored(view);
            const shown = handlers.find(([name]) => name === 'pageshow')[1];
            shown({persisted: false});  // the first load
            expect(reloads).toEqual([]);
            shown({persisted: true});   // back to a page that was kept: its discovery had stopped
            expect(reloads).toEqual([true]);
        });

        it('closes itself on an answer that comes after the wait, which the host gave once it showed the panel again', async () => {
            const status = document.createElement('p');
            const closed = [];
            const view = {BroadcastChannel, setTimeout: (run, delay) => setTimeout(run, delay), clearTimeout: id => clearTimeout(id), opener: {},
                close: () => closed.push(true)};
            const host = new BroadcastChannel(inspectorChannel);
            host.addEventListener('message', ({data}) => {
                if (data?.type === 'attach') setTimeout(() => host.postMessage({type: 'attached', id: data.id, closer: 'self'}), 80);
            });
            try {
                expect(await attachAgain(view, null, 'cpp.watches', status, {host: 'host-a', wait: 20})).toBeFalse();
                expect(status.textContent).toContain('did not answer');
                expect(closed).toEqual([]);
                await new Promise(resolve => setTimeout(resolve, 150));
                expect(closed).toEqual([true]);  // the late answer closed it
                expect(status.textContent).toBe('');
            } finally { host.close(); }
        });

        it('leaves its closing to the host page when the host answers that it closes it', async () => {
            const status = document.createElement('p');
            const calls = [], closed = [];
            const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return true; }};
            for (const opener of [{}, null]) {
                const view = {BroadcastChannel, setTimeout: (run, delay) => setTimeout(run, delay), clearTimeout: id => clearTimeout(id), opener,
                    close: () => closed.push(true)};
                const host = new BroadcastChannel(inspectorChannel);
                host.addEventListener('message', ({data}) => host.postMessage({type: 'attached', id: data.id, closer: 'host'}));
                try { expect(await attachAgain(view, bridge, 'cpp.functions', status, {host: 'host-a'})).toBeTrue(); }
                finally { host.close(); }
            }
            // Neither a tab nor a native window closes itself: a detach meanwhile may have reused it.
            expect(closed).toEqual([]);
            expect(calls).toEqual([]);
        });

        it('closes its native window through C++ when it has no opener', async () => {
            const status = document.createElement('p');
            const calls = [];
            const bridge = {cppFunction: name => async (...args) => { calls.push([name, ...args]); return true; }};
            const view = {BroadcastChannel, setTimeout: (run, delay) => setTimeout(run, delay), clearTimeout: id => clearTimeout(id), opener: null, close: () => {}};
            const host = new BroadcastChannel(inspectorChannel);
            host.addEventListener('message', ({data}) => host.postMessage({type: 'attached', id: data.id}));
            try { expect(await attachAgain(view, bridge, 'cpp.functions', status, {host: 'host-a'})).toBeTrue(); }
            finally { host.close(); }
            expect(calls).toEqual([['webfront.inspector.closeWindow', 'cpp.functions', 'host-a']]);
            // Without the identifier of its page, the window does not ask anybody.
            expect(await attachAgain(view, bridge, 'cpp.functions', status)).toBeFalse();
            expect(calls.length).toBe(1);
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

        it('are listed again when the registry changes between two pages, so that a removed watch is not kept', async () => {
            const registry = memoryWatches(2);
            const ids = ['a', 'b', 'c', 'd'].map(name => registry.add(name));
            let removed = false;
            const bridge = {cppFunction: () => async (...args) => {
                const answer = registry.answer(...args);
                // Between the first and the second page, a watch already read is removed.
                if (!removed && args[1] === 0) { removed = true; registry.remove(ids[0]); }
                return answer;
            }};
            const {watches, revision} = await listWatches(bridge, 0);
            expect(watches.map(watch => watch.name)).toEqual(['b', 'c', 'd']);
            expect(revision).toBe(6);
            // The next poll with that revision sees no change, and the list is right.
            expect(registry.answer(revision, 0, 0)[3]).toBeFalse();
        });

        it('give up a listing while the registry keeps changing, with a bounded number of reads', async () => {
            const registry = memoryWatches(1);
            ['a', 'b'].forEach(name => registry.add(name));
            // Each read adds a watch: no page can ever catch up with the registry.
            const bridge = {cppFunction: () => async (...args) => {
                const answer = registry.answer(...args);
                registry.add(`x${registry.calls.length}`);
                return answer;
            }};
            expect(await listWatches(bridge, 0)).toBeNull();
            expect(registry.calls.filter(([, after]) => after === 0).length).toBe(listingAttempts);
            expect(registry.calls.length).toBeLessThanOrEqual(2 * listingAttempts);  // never reads to the end of a growing list
        });

        it('keep the watches shown when a listing gives up, and show the list once a read is stable', async () => {
            const registry = memoryWatches(1);
            registry.add('a');
            let churning = true;
            const bridge = {cppFunction: () => async (...args) => {
                const answer = registry.answer(...args);
                if (churning) registry.add(`x${registry.calls.length}`);
                return answer;
            }};
            const changes = [];
            const discovery = createWatchDiscovery({bridge, onChange: change => changes.push(change.watches.map(watch => watch.name)), timers: {setTimeout: () => 0, clearTimeout: () => {}}});
            await discovery.start();
            expect(changes).toEqual([]);  // nothing shown from an unstable read
            churning = false;
            await discovery.start();
            expect(changes.length).toBe(1);
            expect(changes[0][0]).toBe('a');
            expect(changes[0].length).toBe((await listWatches(bridge, 0)).watches.length);  // the complete list
            discovery.stop();
        });

        it('send no further read and show nothing read after a pause', async () => {
            const registry = memoryWatches(1);
            ['a', 'b', 'c'].forEach(name => registry.add(name));
            let discovery, pauses = 1;
            const bridge = {cppFunction: () => async (...args) => {
                const answer = registry.answer(...args);
                if (args[2] === 0 && pauses-- > 0) discovery.pause();  // the user pauses while the revision is asked
                return answer;
            }};
            const changes = [];
            discovery = createWatchDiscovery({bridge, onChange: change => changes.push(change), timers: {setTimeout: () => 0, clearTimeout: () => {}}});
            await discovery.start();
            expect(registry.calls.length).toBe(1);  // only the revision: no page after the pause
            expect(changes).toEqual([]);
            // Resumed, it reads again from the revision shown (none yet), and shows the list.
            discovery.resume();
            await discovery.start();
            expect(changes.length).toBe(1);
            expect(changes[0].watches.map(watch => watch.name)).toEqual(['a', 'b', 'c']);
            discovery.stop();
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

        it('report every successful read, so that an error is cleared once the reads work again', async () => {
            jasmine.clock().install();
            try {
                const registry = memoryWatches();
                registry.add('grid');
                let fail = false;
                const events = [];
                const bridge = {cppFunction: () => async (...args) => {
                    if (fail) throw new Error('lost');
                    return registry.answer(...args);
                }};
                const discovery = createWatchDiscovery({bridge, onChange: () => events.push('change'), onError: () => events.push('error'),
                    onRead: watches => events.push(`read ${watches.length}`), period: 250});
                await discovery.start();
                expect(events).toEqual(['change', 'read 1']);
                fail = true;
                jasmine.clock().tick(250);
                await discovery.settled();
                fail = false;
                // The registry has not changed: the read finds nothing new, and still says that it succeeded.
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(events).toEqual(['change', 'read 1', 'error', 'read 1']);
                discovery.stop();
            } finally { jasmine.clock().uninstall(); }
        });

        it('keep one read at a time when paused and resumed while a read is in progress', async () => {
            jasmine.clock().install();
            try {
                const registry = memoryWatches();
                registry.add('grid');
                let hold = null, inFlight = 0, most = 0;
                const bridge = {cppFunction: () => async (...args) => {
                    ++inFlight;
                    most = Math.max(most, inFlight);
                    try {
                        if (hold) await hold.promise;
                        return registry.answer(...args);
                    } finally { --inFlight; }
                }};
                const discovery = createWatchDiscovery({bridge, onChange: () => {}, period: 250});
                await discovery.start();
                // The next read waits for its answer; meanwhile the panel is paused and resumed, and time goes on.
                let release;
                hold = {promise: new Promise(resolve => { release = resolve; })};
                jasmine.clock().tick(250);
                await Promise.resolve();
                expect(inFlight).toBe(1);
                discovery.pause();
                discovery.resume();
                jasmine.clock().tick(1000);
                await Promise.resolve();
                expect(inFlight).toBe(1);
                hold = null;
                release();
                await discovery.settled();
                // The read that ended schedules the next one: one chain, one read per period.
                registry.calls.length = 0;
                jasmine.clock().tick(250);
                await discovery.settled();
                jasmine.clock().tick(250);
                await discovery.settled();
                expect(registry.calls.length).toBe(2);
                expect(most).toBe(1);
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
