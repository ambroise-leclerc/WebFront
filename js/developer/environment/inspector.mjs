// The page of the inspectors (#680, ADR-027 section 8): inspector.html?panel=<id> shows one panel and talks to C++ through the
// bridge. In a window or a tab of its own it opens its own connection; in the floating window of a page (?frame=1) it opens
// none and borrows that page's bridge, since the adapters give the proof of the view to the main frame only. The panel of the
// watches lists them by webfront.inspector.watches and follows the registrations and removals (section 3, "Découverte"); the
// tree of each is in inspector-watches.mjs (#682). The panels of the functions and of the browsers are in
// inspector-registry.mjs (#681, section 7), the one of the packages in inspector-packages.mjs (#683), the one of the bridge
// in the page in inspector-bridge.mjs (#684).
import {inspectorPanels as panels, panelTitle, inspectorChannel as channelName} from './menu.mjs';
import {showFunctionsPanel, showBrowsersPanel, registryStyles, periods, formatPeriod, keptPeriod, keepPeriod, createPoller} from './inspector-registry.mjs';
import {showPackagesPanel, packagesStyles} from './inspector-packages.mjs';
import {createWatchTree, createTreeView, watchTreeStyles} from './inspector-watches.mjs';
import {showBridgePanel, bridgeSource, bridgeStyles} from './inspector-bridge.mjs';

// What a page of the application's origin reads a published state with, as a detached Bridge panel does.
export {readPublishedState} from './inspector-bridge.mjs';

// --- Bridge ----------------------------------------------------------------------------------------------------

/**
 * The bridge of this page: the host page's when it is the frame of a floating window (`frame` set, and a parent of the
 * same origin that has one), else its own, by loading WebFront.js, which connects when it loads. Resolves once ready.
 */
export async function pageBridge(view = window, {frame = false} = {}) {
    if (frame && view.parent !== view) {
        // The bridge the host was created with, which may not be the page's global webFront: the frame then reads the
        // same connection as its host, whose access was checked, and never another one.
        let borrowed = null;
        try { borrowed = view.parent.webfrontInspectorHost?.bridge ?? null; }
        catch { /* another origin: never borrowed */ }
        if (!borrowed) throw new Error('The inspector\'s frame has no host page to borrow the bridge from');
        await borrowed.ready;
        return borrowed;
    }
    if (!view.webFront) {
        const page = view.document;
        await new Promise((resolve, reject) => {
            const script = page.createElement('script');
            script.src = '/WebFront.js';
            script.onload = resolve;
            script.onerror = () => reject(new Error('Cannot load WebFront.js'));
            page.head.append(script);
        });
    }
    await view.webFront.ready;
    return view.webFront;
}

// --- Watches ---------------------------------------------------------------------------------------------------

export const watchesFunction = 'webfront.inspector.watches';
/** The period of the discovery, as the inspector reads its watches (ADR-027: 250 ms by default). */
export const defaultPeriod = 250;

/** A descriptor of webfront.inspector.watches as an object; null when it is not one. */
export function readDescriptor(tuple) {
    if (!Array.isArray(tuple) || tuple.length !== 5) return null;
    const [id, name, kind, location, writable] = tuple;
    if (!Number.isSafeInteger(id) || id <= 0 || typeof name !== 'string' || typeof kind !== 'string' || typeof location !== 'string'
        || typeof writable !== 'boolean') return null;
    return {id, name, kind, location, writable};
}

/** A page of webfront.inspector.watches: {descriptors, next, revision, changed}; throws when it is not one. */
export function readPage(answer) {
    if (!Array.isArray(answer) || answer.length !== 4) throw new Error('The inspectors gave a page that this environment does not read');
    const [list, next, revision, changed] = answer;
    const descriptors = (Array.isArray(list) ? list : []).map(readDescriptor);
    if (descriptors.includes(null) || !Number.isSafeInteger(next) || !Number.isSafeInteger(revision) || typeof changed !== 'boolean')
        throw new Error('The inspectors gave a page that this environment does not read');
    return {descriptors, next, revision, changed};
}

/** How many times a listing starts again because the registry changed while it was read, before it gives up. */
export const listingAttempts = 8;

/**
 * Every live watch, page after page from the start, with the revision of the registry they describe. The pages give the
 * revision of the first one: a page that answers "changed" means that a watch was registered or removed while the list was
 * read, so the pages already read may name a removed watch, and the listing starts again. After `listingAttempts` such
 * changes it gives up and resolves with null: the list shown stays, and the next poll, which finds the registry changed,
 * tries again. Only a stable read gives a list, always a complete one. `active()` is asked before each page: once it is
 * false (the inspector paused or closed) the listing stops and resolves with null, having read no further.
 */
export async function listWatches(bridge, revision = 0, {active = () => true} = {}) {
    for (let attempt = 0; attempt < listingAttempts; ++attempt) {
        if (!active()) return null;
        const first = readPage(await bridge.cppFunction(watchesFunction)(revision, 0, 200));
        const watches = [...first.descriptors];
        let next = first.next, stable = true;
        while (stable && next) {
            if (!active()) return null;
            const page = readPage(await bridge.cppFunction(watchesFunction)(first.revision, next, 200));
            watches.push(...page.descriptors);
            stable = !page.changed;
            next = page.next;
        }
        if (stable) return {watches, revision: first.revision};
        revision = 0;
    }
    return null;
}

/** What changed between the watches shown (`known`, a Map by id) and a new list: the descriptors to add, the ids to remove. */
export function reconcile(known, listed) {
    const ids = new Set(listed.map(watch => watch.id));
    return {added: listed.filter(watch => !known.has(watch.id)), removed: [...known.keys()].filter(id => !ids.has(id))};
}

/**
 * Follows the watches of the application: asks the revision of the registry every `period` with a count of 0, reads the
 * list again when it changed, and calls `onChange({added, removed, watches})`. `forget(id)` removes a watch that a call
 * found gone (a refusal `webfront.inspector.unknown`). `pause()` and `resume()` stop and restart the reads; `stop()` ends them.
 * `onError(error)` receives a failed read; the discovery keeps trying at the next period. `onRead(watches)` follows every read
 * that succeeds, changed or not, so that an error shown earlier can be cleared once the reads work again.
 */
export function createWatchDiscovery({bridge, onChange, onError = () => {}, onRead = () => {}, period = defaultPeriod, timers = globalThis}) {
    const known = new Map();
    let revision = 0, timer = null, paused = false, stopped = false, running = null;
    const apply = listed => {
        const change = reconcile(known, listed);
        for (const watch of change.added) known.set(watch.id, watch);
        for (const id of change.removed) known.delete(id);
        if (change.added.length || change.removed.length) onChange({...change, watches: [...known.values()]});
    };
    // Paused or stopped: no further request, and nothing read after the pause is shown. The revision moves only with a list
    // that is shown, so that a resumption reads again from the one shown.
    const active = () => !paused && !stopped;
    async function poll() {
        const {changed, revision: current} = readPage(await bridge.cppFunction(watchesFunction)(revision, 0, 0));
        if (!active()) return;
        if (changed || current !== revision) {
            const listed = await listWatches(bridge, current, {active});
            if (!active()) return;
            if (listed) {
                revision = listed.revision;
                apply(listed.watches);
            }
        }
        onRead([...known.values()]);
    }
    // One chain of reads at a time: a read in progress schedules the next one when it ends, so nothing else does meanwhile
    // (a pause then a resumption during a read would start a second chain, whose results could overtake the first's).
    function schedule() {
        if (stopped || paused || timer !== null || running !== null) return;
        timer = timers.setTimeout(() => {
            timer = null;
            running = poll().catch(onError).finally(() => {
                running = null;
                schedule();
            });
        }, period);
    }
    const discovery = {
        get watches() { return [...known.values()]; },
        get paused() { return paused; },
        /** Reads now, then at each period. */
        async start() {
            running = poll().catch(onError);
            await running;
            running = null;
            schedule();
        },
        /** Forgets a watch that a call found gone, as a removal from the registry would. */
        forget(id) {
            if (!known.delete(id)) return;
            onChange({added: [], removed: [id], watches: [...known.values()]});
        },
        pause() {
            paused = true;
            timers.clearTimeout(timer);
            timer = null;
        },
        resume() {
            paused = false;
            schedule();
        },
        stop() {
            stopped = true;
            timers.clearTimeout(timer);
            timer = null;
        },
        /** Resolves once the read in progress, if any, is over. */
        settled: () => running ?? Promise.resolve(),
    };
    return discovery;
}

// --- Page ------------------------------------------------------------------------------------------------------

const styles = `
:root{color-scheme:dark}
body{margin:0;background:#141821;color:#e8edf4;font:13px/1.35 system-ui}
header{display:flex;flex-wrap:wrap;align-items:center;gap:4px 8px;padding:4px 8px;background:#202737;border-bottom:1px solid #46536a}
header h1{flex:1 1 auto;margin:0;font-size:13px;font-weight:600}
header label{display:flex;align-items:center;gap:4px;color:#a9b3c4;font-size:12px}
header button,header select{min-height:26px;padding:0 8px;border:1px solid #46536a;border-radius:4px;background:#2f3a50;color:inherit;font:inherit;cursor:pointer}
header select{padding:0 4px}
header button:focus-visible,header select:focus-visible{outline:2px solid #d7fa85;outline-offset:1px}
main{padding:4px 8px}
.status{color:#a9b3c4;font-size:12px;margin:2px 0 4px}
.status:empty{display:none}
.error{color:#ffbd2e}
ul.watches{list-style:none;margin:0;padding:0}
ul.watches li{display:grid;grid-template-columns:auto 1fr;gap:0 8px;padding:2px 4px;border-bottom:1px solid #2f3a50}
ul.watches .name{font-weight:600}
ul.watches .kind{color:#d7fa85;font-family:ui-monospace,monospace;font-size:12px;justify-self:end}
ul.watches .location{grid-column:1/3;color:#a9b3c4;font-size:11px;overflow-wrap:anywhere}
ul.watches .writable{color:#8fd0ff;font-size:11px}
@media (pointer:coarse){header button,header select{min-height:44px;padding:0 12px}}`;

/**
 * Renders the list of the watches in `list`, one item per watch, kept by id so that a removal removes its item. With
 * `onToggle(watch, item)`, each item has a button that unfolds the tree of its watch.
 */
export function renderWatches(list, watches, {onToggle = null} = {}) {
    const page = list.ownerDocument;
    const items = new Map([...list.children].map(item => [Number(item.dataset.watch), item]));
    const ids = new Set(watches.map(watch => watch.id));
    for (const [id, item] of items) if (!ids.has(id)) item.remove();
    for (const watch of watches) {
        if (items.has(watch.id)) continue;
        const item = page.createElement('li');
        item.dataset.watch = String(watch.id);
        const cell = (className, text) => {
            const element = page.createElement('span');
            element.className = className;
            element.textContent = text;
            return element;
        };
        const head = page.createElement('div');
        head.className = 'head';
        if (onToggle) {
            const toggle = page.createElement('button');
            toggle.type = 'button';
            toggle.className = 'toggle';
            toggle.textContent = '▸';
            toggle.setAttribute('aria-expanded', 'false');
            toggle.setAttribute('aria-label', `Show the content of ${watch.name}`);
            toggle.addEventListener('click', () => onToggle(watch, item));
            head.append(toggle);
        }
        head.append(cell('name', watch.name), cell('kind', watch.kind));
        if (watch.writable) head.append(cell('writable', 'writable'));
        // On the line of its name, cut at its start when the panel is narrow: its title gives it whole.
        const location = cell('location', '');
        const path = page.createElement('span');
        path.dir = 'ltr';
        path.textContent = watch.location;
        location.append(path);
        location.title = watch.location;
        head.append(location);
        item.append(head);
        list.append(item);
    }
}

/** Closes this window once the page that opened it is gone; nothing for a window that no page opened. */
export function watchOpener(view, period = 1000) {
    const opener = view.opener;
    if (!opener) return null;
    const watcher = view.setInterval(() => { if (opener.closed) view.close(); }, period);
    view.addEventListener('pagehide', () => view.clearInterval(watcher));
    return watcher;
}

/**
 * Loads the page again when the browser restores it from its back-forward cache. The restored page is resumed without
 * main(): the discovery of the watches and the watch of the opener, stopped when it was hidden, would not start again, and
 * the connection of a tab may have been closed meanwhile. Loading it again starts all of them anew.
 */
export function reloadWhenRestored(view) {
    view.addEventListener('pageshow', event => { if (event.persisted) view.location.reload(); });
}

/**
 * Shows the panel named by the page's URL. In a frame it borrows the host page's bridge; elsewhere it has its own
 * connection, offers to attach the panel to the host page again, and closes itself when the page that opened it is gone.
 */
export async function main(view = window) {
    const page = view.document;
    const params = new URL(view.location.href).searchParams;
    const id = params.get('panel') ?? 'cpp.watches';
    const framed = params.get('frame') === '1';
    page.title = `WebFront inspector: ${panelTitle(id)}`;
    reloadWhenRestored(view);
    const style = page.createElement('style');
    style.textContent = styles + registryStyles + packagesStyles + watchTreeStyles + bridgeStyles;
    page.head.append(style);
    const header = page.createElement('header');
    const heading = page.createElement('h1');
    heading.textContent = panelTitle(id);
    header.append(heading);
    const body = page.createElement('main');
    const status = page.createElement('p');
    status.className = 'status';
    status.setAttribute('role', 'status');
    body.append(status);
    page.body.replaceChildren(header, body);
    if (!panels.has(id)) {
        status.textContent = `Unknown inspector panel: ${id}`;
        return null;
    }

    // A tab that the main page opened closes with it, as a secondary window closes with the main window (#616): watched
    // before the bridge is awaited, so that a tab whose connection fails or is still opening closes too.
    if (!framed) watchOpener(view);

    let bridge;
    try { bridge = await pageBridge(view, {frame: framed}); }
    catch (error) {
        status.textContent = `The inspector cannot reach the application: ${error?.message ?? error}`;
        status.classList.add('error');
        return null;
    }

    // Detached, the panel can go back to the floating window of the page that holds the tab.
    if (!framed) {
        const attach = page.createElement('button');
        attach.type = 'button';
        attach.className = 'attach';
        attach.textContent = 'Attach';
        attach.title = 'Show this panel in the floating window of the main page';
        header.append(attach);
        // Its own status: the reads of the watches, which clear theirs, never clear why the panel could not be attached.
        const attachStatus = page.createElement('p');
        attachStatus.className = 'status attach-status';
        attachStatus.setAttribute('role', 'status');
        body.prepend(attachStatus);
        attach.addEventListener('click', () => attachAgain(view, bridge, id, attachStatus, {host: params.get('host')}));
    }

    if (id === 'cpp.functions' || id === 'cpp.browsers') {
        const show = id === 'cpp.functions' ? showFunctionsPanel : showBrowsersPanel;
        return {bridge, ...await show({page, view, header, body, status, bridge})};
    }
    if (id === 'typescript.packages') return {bridge, ...await showPackagesPanel({page, view, header, body, status, bridge})};
    if (id === 'typescript.bridge')
        return {bridge, ...await showBridgePanel({page, view, header, body, status, source: bridgeSource({view, bridge, framed, host: params.get('host')})})};
    const list = page.createElement('ul');
    list.className = 'watches';
    list.setAttribute('aria-label', 'Watches');
    body.append(list);
    const pause = page.createElement('button');
    pause.type = 'button';
    pause.className = 'pause';
    pause.textContent = 'Pause';
    header.insertBefore(pause, header.children[1] ?? null);
    const periodLabel = page.createElement('label');
    periodLabel.textContent = 'Refresh every ';
    const period = page.createElement('select');
    period.className = 'period';
    const chosen = keptPeriod(view);
    for (const value of periods) {
        const option = page.createElement('option');
        option.value = String(value);
        option.textContent = formatPeriod(value);
        option.selected = value === chosen;
        period.append(option);
    }
    periodLabel.append(period);
    header.insertBefore(periodLabel, pause);
    const describe = watches => {
        status.classList.remove('error');
        status.textContent = watches.length ? '' : 'The application declares no watch (app.watch).';
    };
    const denied = error => error?.code === 'webfront.inspector.denied'
        ? 'The inspectors answer only a view of the application: this page is not admitted.' : null;

    // The trees that are unfolded, by the identifier of their watch. Each is read again at the period, unless paused.
    const trees = new Map();
    const close = id => {
        const shown = trees.get(id);
        if (!shown) return;
        shown.tree.stop();
        shown.view.stop();
        shown.element.remove();
        trees.delete(id);
    };
    const toggle = async (watch, item) => {
        const button = item.querySelector('button.toggle');
        if (trees.has(watch.id)) {
            close(watch.id);
            button.textContent = '▸';
            button.setAttribute('aria-expanded', 'false');
            return;
        }
        const element = page.createElement('ul');
        item.append(element);
        let shown = null;
        const tree = createWatchTree({
            bridge,
            watch: watch.id,
            maxMessageBytes: bridge.maxMessageBytes ?? Infinity,
            onUpdate: () => shown?.render(),
            // A watch that C++ no longer knows leaves the list at once, without waiting for the next discovery.
            onGone: () => { close(watch.id); discovery.forget(watch.id); },
        });
        shown = createTreeView({tree, list: element, view, name: watch.name});
        trees.set(watch.id, {tree, view: shown, element});
        button.textContent = '▾';
        button.setAttribute('aria-expanded', 'true');
        try { await tree.open(); }
        catch (error) {
            if (trees.has(watch.id)) {
                status.classList.add('error');
                status.textContent = denied(error) ?? `The watch cannot be read: ${error?.message ?? error}`;
            }
        }
    };
    const discovery = createWatchDiscovery({
        bridge,
        onChange: ({watches, removed}) => {
            for (const id of removed) close(id);
            renderWatches(list, watches, {onToggle: toggle});
        },
        // Every read that succeeds says so, also after an error and when nothing changed.
        onRead: describe,
        onError: error => {
            status.classList.add('error');
            status.textContent = denied(error) ?? `The watches cannot be read: ${error?.message ?? error}`;
        },
        timers: view,
    });
    // One pass of differences per tree and per period; a tree that is busy (the user unfolds, writes) is skipped this time.
    const refresher = createPoller({
        read: async () => { await Promise.all([...trees.values()].map(({tree}) => tree.refresh().catch(() => false))); return true; },
        onData: () => {},
        period: chosen,
        timers: view,
    });
    period.addEventListener('change', () => {
        const value = Number(period.value);
        keepPeriod(view, value);
        refresher.setPeriod(value);
    });
    pause.addEventListener('click', () => {
        if (discovery.paused) { discovery.resume(); refresher.resume(); }
        else { discovery.pause(); refresher.pause(); }
        pause.textContent = discovery.paused ? 'Resume' : 'Pause';
    });
    view.addEventListener('pagehide', () => {
        discovery.stop();
        refresher.stop();
        for (const id of [...trees.keys()]) close(id);
    });
    await discovery.start();
    refresher.start();
    return {bridge, discovery, list, trees, refresher, toggle};
}

/**
 * Asks the page that detached the panel (`host`, the identifier its address carries) to show it again. That page closes this
 * window itself when it can (a native window, or a tab it opened); otherwise it answers that this window closes itself: a tab
 * by itself, a native window through C++. Another page of the same origin does not answer. Without an answer within `wait`
 * the window stays and says why in `status`; an answer that comes later still closes it.
 */
export async function attachAgain(view, bridge, id, status, {host = null, wait = 1000} = {}) {
    if (typeof view.BroadcastChannel !== 'function' || !host) {
        status.textContent = 'This window cannot reach the page that opened it: close it and open the panel from the menu.';
        return false;
    }
    status.textContent = '';
    const channel = new view.BroadcastChannel(channelName);
    const request = `${id}:${Date.now()}:${Math.random()}`;
    // The channel listens until the answer, or until this page goes: an answer that comes after the wait still closes this
    // window, since the host shows the panel again by then (else the panel would be shown twice).
    const close = () => channel.close();
    view.addEventListener?.('pagehide', close, {once: true});
    const finish = async answer => {
        close();
        status.textContent = '';
        // The host closes this window itself, ordered with a detach the user may make meanwhile, which reuses or reopens it:
        // closing it from here could close the window of that newer detach.
        if (answer.closer === 'host') return true;
        if (view.opener) view.close();
        else await bridge.cppFunction('webfront.inspector.closeWindow')(id, host).catch(() => view.close());
        return true;
    };
    return new Promise(resolve => {
        let late = false;
        const timer = view.setTimeout(() => {
            late = true;
            status.textContent = 'The main page did not answer: open the panel from the menu of its tab.';
            resolve(false);
        }, wait);
        channel.addEventListener('message', ({data}) => {
            if (data?.type !== 'attached' || data.id !== request) return;
            view.clearTimeout(timer);
            finish(data).then(done => { if (!late) resolve(done); });
        });
        channel.postMessage({type: 'attach', panel: id, host, id: request});
    });
}
