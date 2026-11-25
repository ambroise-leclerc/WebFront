// The page of the inspectors (#680, ADR-027 section 8): inspector.html?panel=<id> shows one panel and talks to C++ through the
// bridge. In a window or a tab of its own it opens its own connection; in the floating window of a page (?frame=1) it opens
// none and borrows that page's bridge, since the adapters give the proof of the view to the main frame only. The panel of the
// watches lists them by webfront.inspector.watches and follows the registrations and removals (section 3, "Découverte").
import {inspectorPanels as panels, panelTitle, inspectorChannel as channelName} from './menu.mjs';

// --- Bridge ----------------------------------------------------------------------------------------------------

/**
 * The bridge of this page: the host page's when it is the frame of a floating window (`frame` set, and a parent of the
 * same origin that has one), else its own, by loading WebFront.js, which connects when it loads. Resolves once ready.
 */
export async function pageBridge(view = window, {frame = false} = {}) {
    if (frame && view.parent !== view) {
        let borrowed = null;
        try { borrowed = view.parent.webFront; }
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
 * changes, the list is given with the revision 0, which the next poll always finds changed.
 */
export async function listWatches(bridge, revision = 0) {
    for (let attempt = 0; attempt < listingAttempts; ++attempt) {
        const first = readPage(await bridge.cppFunction(watchesFunction)(revision, 0, 200));
        const watches = [...first.descriptors];
        let next = first.next, stable = true;
        while (stable && next) {
            const page = readPage(await bridge.cppFunction(watchesFunction)(first.revision, next, 200));
            watches.push(...page.descriptors);
            stable = !page.changed;
            next = page.next;
        }
        if (stable) return {watches, revision: first.revision};
        revision = 0;
    }
    const {watches} = await listWatchesOnce(bridge);
    return {watches, revision: 0};
}

async function listWatchesOnce(bridge) {
    const watches = [];
    for (let after = 0;;) {
        const page = readPage(await bridge.cppFunction(watchesFunction)(0, after, 200));
        watches.push(...page.descriptors);
        if (!page.next) return {watches};
        after = page.next;
    }
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
    async function poll() {
        const {changed, revision: current} = readPage(await bridge.cppFunction(watchesFunction)(revision, 0, 0));
        if (changed || current !== revision) {
            const listed = await listWatches(bridge, current);
            revision = listed.revision;
            apply(listed.watches);
        }
        onRead([...known.values()]);
    }
    function schedule() {
        if (stopped || paused || timer !== null) return;
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
body{margin:0;background:#141821;color:#e8edf4;font:14px system-ui}
header{display:flex;align-items:center;gap:8px;padding:8px 12px;background:#202737;border-bottom:1px solid #46536a}
header h1{flex:1 1 auto;margin:0;font-size:14px;font-weight:600}
header button{min-height:32px;padding:0 10px;border:1px solid #46536a;border-radius:4px;background:#2f3a50;color:inherit;font:inherit;cursor:pointer}
header button:focus-visible{outline:2px solid #d7fa85;outline-offset:1px}
main{padding:8px 12px}
.status{color:#a9b3c4;font-size:12px;margin:4px 0 8px}
.status:empty{display:none}
.error{color:#ffbd2e}
ul.watches{list-style:none;margin:0;padding:0}
ul.watches li{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;padding:8px;border-bottom:1px solid #2f3a50}
ul.watches .name{font-weight:600}
ul.watches .kind{color:#d7fa85;font-family:ui-monospace,monospace;font-size:12px;justify-self:end}
ul.watches .location{grid-column:1/3;color:#a9b3c4;font-size:12px;overflow-wrap:anywhere}
ul.watches .writable{color:#8fd0ff;font-size:12px}`;

const placeholders = {
    'cpp.functions': 'The functions that the application registered come with the next versions of the environment.',
    'cpp.browsers': 'The connected browsers and the running tasks come with the next versions of the environment.',
    'typescript.packages': 'The tree of the locked packages comes with the next versions of the environment.',
    'typescript.bridge': 'The state of the bridge in the page comes with the next versions of the environment.',
};

/** Renders the list of the watches in `list`, one item per watch, kept by id so that a removal removes its item. */
export function renderWatches(list, watches) {
    const page = list.ownerDocument;
    const items = new Map([...list.children].map(item => [Number(item.dataset.watch), item]));
    for (const [id, item] of items) if (!watches.some(watch => watch.id === id)) item.remove();
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
        item.append(cell('name', watch.name), cell('kind', watch.kind), cell('location', watch.location));
        if (watch.writable) item.append(cell('writable', 'writable'));
        list.append(item);
    }
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
    const style = page.createElement('style');
    style.textContent = styles;
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
        attach.addEventListener('click', () => attachAgain(view, bridge, id, status, {host: params.get('host')}));
        // A tab that the main page opened closes with it, as a secondary window closes with the main window (#616).
        if (view.opener) {
            const opener = view.opener;
            const watcher = view.setInterval(() => { if (opener.closed) view.close(); }, 1000);
            view.addEventListener('pagehide', () => view.clearInterval(watcher));
        }
    }

    if (id !== 'cpp.watches') {
        status.textContent = placeholders[id];
        return {bridge};
    }
    const list = page.createElement('ul');
    list.className = 'watches';
    list.setAttribute('aria-label', 'Watches');
    body.append(list);
    const pause = page.createElement('button');
    pause.type = 'button';
    pause.className = 'pause';
    pause.textContent = 'Pause';
    header.insertBefore(pause, header.children[1] ?? null);
    const describe = watches => {
        status.classList.remove('error');
        status.textContent = watches.length ? '' : 'The application declares no watch (app.watch).';
    };
    const discovery = createWatchDiscovery({
        bridge,
        onChange: ({watches}) => renderWatches(list, watches),
        // Every read that succeeds says so, also after an error and when nothing changed.
        onRead: describe,
        onError: error => {
            status.classList.add('error');
            status.textContent = error?.code === 'webfront.inspector.denied'
                ? 'The inspectors answer only a view of the application: this page is not admitted.'
                : `The watches cannot be read: ${error?.message ?? error}`;
        },
        timers: view,
    });
    pause.addEventListener('click', () => {
        if (discovery.paused) discovery.resume(); else discovery.pause();
        pause.textContent = discovery.paused ? 'Resume' : 'Pause';
    });
    view.addEventListener('pagehide', () => discovery.stop());
    await discovery.start();
    return {bridge, discovery, list};
}

/**
 * Asks the page that detached the panel (`host`, the identifier its address carries) to show it again, and closes this
 * window once that page answered: a native window through C++, a tab by itself. Another page of the same origin does not
 * answer. Without an answer the window stays, and says why.
 */
export async function attachAgain(view, bridge, id, status, {host = null, wait = 1000} = {}) {
    if (typeof view.BroadcastChannel !== 'function' || !host) {
        status.textContent = 'This window cannot reach the page that opened it: close it and open the panel from the menu.';
        return false;
    }
    const channel = new view.BroadcastChannel(channelName);
    const request = `${id}:${Date.now()}:${Math.random()}`;
    const answered = await new Promise(resolve => {
        const timer = view.setTimeout(() => resolve(false), wait);
        channel.addEventListener('message', ({data}) => {
            if (data?.type !== 'attached' || data.id !== request) return;
            view.clearTimeout(timer);
            resolve(true);
        });
        channel.postMessage({type: 'attach', panel: id, host, id: request});
    });
    channel.close();
    if (!answered) {
        status.textContent = 'The main page did not answer: open the panel from the menu of its tab.';
        return false;
    }
    if (view.opener) view.close();
    else await bridge.cppFunction('webfront.inspector.closeWindow')(id, host).catch(() => view.close());
    return true;
}
