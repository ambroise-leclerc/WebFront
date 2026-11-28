// The host of the inspectors (#680, ADR-027 section 8): opens the page inspector.html of the environment in the container that
// gives it access to C++. One page serves every container: a native secondary window (its own proven connection), a tab of
// the system browser (its own connection, admitted only by trustLoopbackClients), or a floating window of this page whose
// frame opens no connection and borrows this page's bridge. Each panel can be detached and attached again; the choice and
// the floating window's place are kept in the project, as the terminal's place is.
import {readProject, call} from './io.mjs';
import {clampRect, step, minimumSize, windowControls} from './workspace.mjs';
import {inspectorPanels as panels, panelTitle, inspectorChannel, inspectorMoves, makeMoveButton} from './menu.mjs';
import {createBridgeMonitor, createBridgePublisher} from './inspector-bridge.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export {panels, panelTitle};

// --- Containers ------------------------------------------------------------------------------------------------

/**
 * Where a detached panel can go, from what webfront.inspector.containers() allowed (`containers`, null when nothing reaches
 * the inspectors) and whether this page is the view of an embedded frontend (`embedded`): a native secondary window when the
 * frontend opens one for this admitted page; else a tab of the system browser when new loopback connections are admitted
 * and this page is not an embedded view, where window.open() would open a frame without the proof; else null (ADR-027,
 * section 8).
 */
export function detachedContainer(containers, {embedded = true} = {}) {
    if (!containers?.host) return null;
    if (containers.secondaryWindow) return 'window';
    if (containers.newConnections && !embedded) return 'tab';
    return null;
}

export const hostReasons = {
    noDetached: 'No detached window can reach the inspectors from this page: this frontend opens no secondary window, '
        + 'and a tab of the system browser would be refused (it needs the system browser and WebFrontConfig::trustLoopbackClients).',
    blocked: 'The browser blocked the inspector\'s tab: allow pop-ups for this page, then try again.',
    windowFailed: 'The inspector\'s window could not open',
};

/**
 * The container a panel opens in: its detached container when the project remembers it detached and one is available, else
 * the floating window of this page. Null when nothing reaches the inspectors.
 */
export function containerFor(panel, {containers, embedded, detached}) {
    if (!containers?.host) return null;
    const away = detachedContainer(containers, {embedded});
    return detached.has(panel) && away ? away : 'floating';
}

// --- Remembered choices ----------------------------------------------------------------------------------------

/** The panels kept detached and the floating window's place, kept in the project like the terminal's place. */
export const preferencesPath = '.webfront/developer/inspector-window.json';

/** The choices of a file's text: {detached: Set of panel ids, rect or null}; empty ones when it is not one. */
export function parsePreferences(text) {
    const empty = {detached: new Set(), rect: null};
    try {
        const {format, detached = [], rect = null} = JSON.parse(text);
        if (format !== 1 || !Array.isArray(detached)) return empty;
        const valid = rect && ['x', 'y', 'width', 'height'].every(key => Number.isFinite(rect[key]) && Math.abs(rect[key]) < 1e6)
            && rect.width > 0 && rect.height > 0;
        return {detached: new Set(detached.filter(id => panels.has(id))),
            rect: valid ? {x: rect.x, y: rect.y, width: rect.width, height: rect.height} : null};
    } catch { return empty; }
}

export const formatPreferences = ({detached, rect}) =>
    JSON.stringify({format: 1, detached: [...detached].sort(), rect: rect ?? null}) + '\n';

/**
 * Whether a project file is absent, rather than present but unreadable: the directory that holds it, listed, does not name
 * it, or is itself absent. A failure to list a directory that exists is thrown.
 */
async function projectFileMissing(bridge, path) {
    const slash = path.lastIndexOf('/');
    const directory = slash < 0 ? '' : path.slice(0, slash), name = path.slice(slash + 1);
    try {
        const names = JSON.parse(decoder.decode(await call(bridge, 'list', directory)));
        return !names.includes(name) && !names.includes(`${name}/`);
    } catch (error) {
        if (directory && await projectFileMissing(bridge, directory)) return true;
        throw error;
    }
}

const readPreferencesFunction = 'webfront.inspector.readPreferences';
const missingFunction = error => error?.code === 'webfront.notFound' || /C\+\+ function '[^']*' was not found/.test(String(error?.message));

/**
 * The choices the project has, with the token that a save from them is conditioned on: {preferences, token}. Empty choices
 * without a file; a file that exists but cannot be read throws. The library reads the file and its token under one lock, the
 * token being that of the bytes read: a content that changed and came back meanwhile does not take a save of choices read
 * from another one. A library without webfront.inspector.readPreferences gives the token, then the file.
 */
async function readStoredPreferences(bridge) {
    try {
        const [present, bytes, token] = await bridge.cppFunction(readPreferencesFunction)();
        return {preferences: parsePreferences(present ? decoder.decode(bytes) : ''), token};
    } catch (error) {
        if (!missingFunction(error)) throw error;
    }
    const token = await call(bridge, 'state', preferencesPath);
    let bytes;
    try { bytes = await readProject(bridge, preferencesPath); }
    catch (error) {
        if (await projectFileMissing(bridge, preferencesPath).catch(() => false)) return {preferences: parsePreferences(''), token};
        throw error;
    }
    return {preferences: parsePreferences(decoder.decode(bytes)), token};
}

/** The choices to show: empty ones when they cannot be read, which never prevents the inspectors from opening. */
export async function readPreferences(bridge) {
    try { return (await readStoredPreferences(bridge)).preferences; }
    catch { return parsePreferences(''); }
}

/** The choices after `changes` ({panels: Map of panel id to detached, rect: undefined when unchanged}) apply to `current`. */
export function applyPreferenceChanges(current, {panels, rect}) {
    const detached = new Set(current.detached);
    for (const [id, away] of panels) if (away) detached.add(id); else detached.delete(id);
    return {detached, rect: rect === undefined ? current.rect : rect};
}

// Another page writing the choices, or having changed them since they were read: the change is tried again shortly after.
const preferenceConflict = /changed since it was read|already being written/;

/**
 * Applies `changes` to the choices as the project has them now, committed only if the file did not change meanwhile. A file
 * that cannot be read fails the write: committing this page's changes alone would erase the choices of the other pages.
 */
async function writePreferenceChanges(bridge, changes) {
    for (let attempt = 1; ; ++attempt) {
        try {
            const {preferences, token} = await readStoredPreferences(bridge);
            const text = formatPreferences(applyPreferenceChanges(preferences, changes));
            await bridge.cppFunction('webfront.inspector.savePreferences')(token, encoder.encode(text));
            return;
        } catch (error) {
            if (attempt === 20 || !preferenceConflict.test(error?.message)) throw error;
            await new Promise(resolve => setTimeout(resolve, 10 + Math.random() * 40));
        }
    }
}

/**
 * Writes the choices of this page as changes to those of the project, so that the pages of a project do not undo each
 * other's: whether a panel is detached is written at once, the window's place shortly after it stops moving. Each write
 * reads the file again and applies only what this page changed, and is committed only if the file did not change since
 * it was read (else it is tried again). `saved()` writes what is pending and resolves once it is written; a failed write
 * keeps the inspector usable, and its changes are written with the next one, after any newer change of the same panel or
 * place, which wins.
 */
export function preferencesStore(bridge, delay = 300) {
    let timer = null, pending = {panels: new Map(), rect: undefined}, writes = Promise.resolve();
    const flush = () => {
        clearTimeout(timer);
        timer = null;
        if (!pending.panels.size && pending.rect === undefined) return writes;
        const changes = pending;
        pending = {panels: new Map(), rect: undefined};
        writes = writes.catch(() => {}).then(() => writePreferenceChanges(bridge, changes)).catch(error => {
            // Not written: kept for the next write, under the changes made since, which are newer.
            for (const [id, away] of changes.panels) if (!pending.panels.has(id)) pending.panels.set(id, away);
            if (pending.rect === undefined) pending.rect = changes.rect;
            throw error;
        });
        return writes;
    };
    return {
        /** Starts saving the new detached state immediately. */
        detached(id, away) {
            pending.panels.set(id, away);
            flush().catch(() => {});
        },
        /** Remembers the floating window's place once it stops changing. */
        place(rect) {
            pending.rect = rect;
            clearTimeout(timer);
            timer = setTimeout(() => flush().catch(() => {}), delay);
        },
        saved: () => flush(),
    };
}

/** Where the floating window opens without a remembered place: on the left of the terminal's default place. */
export function defaultInspectorRect(viewport) {
    const width = Math.min(560, viewport.width - 2 * step), height = Math.min(480, viewport.height - 2 * step);
    return clampRect({x: step, y: step, width, height}, viewport);
}

const styles = `
.webfront-inspector-window{position:fixed;z-index:2147483645;display:flex;flex-direction:column;box-sizing:border-box;background:#141821;border:1px solid #46536a;border-radius:8px;box-shadow:0 8px 32px #000a;resize:both;overflow:hidden;max-width:100vw;max-height:100vh;min-width:min(${minimumSize.width}px,100vw);min-height:min(${minimumSize.height}px,100vh)}
.webfront-inspector-window[hidden]{display:none}
.webfront-inspector-bar{display:flex;flex:none;align-items:center;gap:4px;padding:0 4px 0 12px;background:#202737;color:#e8edf4;font:13px system-ui;cursor:move;user-select:none;touch-action:none}
.webfront-inspector-bar>span{flex:1 1 auto;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.webfront-inspector-bar:focus-visible,.webfront-inspector-bar button:focus-visible{outline:2px solid #d7fa85;outline-offset:-2px}
.webfront-inspector-bar button{margin:0;min-width:32px;height:32px;padding:0 8px;border:0;border-radius:4px;background:none;color:inherit;font:13px system-ui;cursor:pointer}
.webfront-inspector-bar button:hover:not([aria-disabled=true]){background:#2f3a50}
.webfront-inspector-bar button[aria-disabled=true]{color:#8d97aa;cursor:default}
.webfront-inspector-close{font-size:20px!important;line-height:1}
.webfront-inspector-detach>svg{display:block;margin:auto;pointer-events:none}
.webfront-inspector-notice{display:flex;gap:8px;align-items:center;padding:6px 12px;background:#3a2f12;color:#f4e3b5;font:12px system-ui}
.webfront-inspector-notice[hidden]{display:none}
.webfront-inspector-notice button{margin:0 0 0 auto;padding:4px 8px;border:1px solid #f4e3b5;border-radius:4px;background:none;color:inherit;font:inherit;cursor:pointer}
.webfront-inspector-frame{flex:1 1 auto;min-height:0;width:100%;border:0;background:#141821}`;

/** Where a page keeps its host identifier: the session storage of its tab, so that a reload keeps its detached windows. */
export const hostKey = 'webfront-inspector-host';

const validHost = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(value);
const randomHost = view => Array.from(view.crypto.getRandomValues(new Uint8Array(12)), byte => byte.toString(16).padStart(2, '0')).join('');

/** Takes the lock `name` for as long as the page lives, when nobody holds it; resolves with whether it did. */
function holdLock(locks, name) {
    return new Promise(resolve => {
        locks.request(name, {ifAvailable: true}, lock => {
            resolve(Boolean(lock));
            return lock ? new Promise(() => {}) : undefined;  // held until the page goes away
        }).catch(() => resolve(false));
    });
}

/**
 * The identifier of this host page (1 to 64 letters, digits, `-` or `_`, which webfront.inspector.openWindow checks), so
 * that a detached panel is attached again only to the page that detached it. A reload keeps it: it is kept in the tab's
 * session storage. That storage is also copied into a page that this one opens, so the identifier is used only by the page
 * that holds its lock (Web Locks, exclusive for the origin, released when the page goes away): a page that finds it held
 * takes a new one. Without Web Locks nothing can tell a reload from a copy, and every load takes a new identifier.
 */
export async function claimHostIdentifier(view) {
    let stored = null;
    try { stored = view.sessionStorage?.getItem(hostKey); }
    catch { /* no storage: a new identifier for this load */ }
    const locks = view.navigator?.locks;
    let claimed = null;
    if (locks) {
        for (let candidate = validHost(stored) ? stored : randomHost(view), attempt = 0; attempt < 4 && !claimed; ++attempt) {
            if (await holdLock(locks, `${hostKey}:${candidate}`)) claimed = candidate;
            else candidate = randomHost(view);
        }
    }
    claimed ??= randomHost(view);
    try { view.sessionStorage?.setItem(hostKey, claimed); }
    catch { /* kept for this load only */ }
    return claimed;
}

/**
 * The inspectors' host for the page that holds the tab. `inspectorUrl` is the URL of inspector.html, `access` resolves to what
 * the menu read (menu.mjs, inspectorAccess), and `embedded` to whether this page may run in an embedded frontend (by default asked to
 * webfront.inspector.embedded(); an error counts as embedded, which never offers a tab). `open(panel)` shows a panel in its
 * container, `detach(panel)` and `attach(panel)` move it, and `reason(panel)` says why it cannot be detached ('' when it
 * can). `windowOpen` replaces window.open (tests). `hostId` names this page in its detached windows (claimHostIdentifier()).
 * `inspected()` gives the bridge whose state the Bridge panel shows, as `{bridge, label, path}`, or null for this page's own.
 */
export async function createInspectorHost({bridge, page, inspectorUrl, access, embedded = null, windowOpen = null, hostId = null, inspected = null}) {
    const view = page.defaultView, root = page.documentElement;
    const self = hostId ?? await claimHostIdentifier(view);
    const viewport = () => ({width: root.clientWidth, height: root.clientHeight});
    const [preferences, containers, isEmbedded] = await Promise.all([
        readPreferences(bridge),
        Promise.resolve(access).then(result => result?.containers ?? null),
        Promise.resolve(embedded ?? bridge.cppFunction('webfront.inspector.embedded')()).then(Boolean, () => true),
    ]);
    const store = preferencesStore(bridge);
    const openTab = windowOpen ?? ((url, name) => view.open(url, name));
    const urlOf = (panel, extra = {}) => {
        const url = new URL(inspectorUrl, view.location.href);
        url.searchParams.set('panel', panel);
        url.searchParams.set('host', self);
        for (const [key, value] of Object.entries(extra)) url.searchParams.set(key, value);
        return url.href;
    };

    // The floating window: one per page, showing one panel at a time.
    const style = page.createElement('style');
    style.textContent = styles;
    page.head.append(style);
    const panel = page.createElement('section');
    panel.className = 'webfront-inspector-window';
    panel.hidden = true;
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'false');
    const bar = page.createElement('div');
    bar.className = 'webfront-inspector-bar';
    bar.tabIndex = 0;
    bar.setAttribute('role', 'group');
    bar.setAttribute('aria-label', 'Inspector window: drag it, or use the arrow keys to move it and Shift with the arrow keys to resize it');
    const title = page.createElement('span');
    const detachButton = page.createElement('button');
    detachButton.type = 'button';
    detachButton.className = 'webfront-inspector-detach';
    makeMoveButton(page, detachButton, 'detach');
    const closeButton = page.createElement('button');
    closeButton.type = 'button';
    closeButton.className = 'webfront-inspector-close';
    closeButton.setAttribute('aria-label', 'Close the inspector');
    closeButton.title = 'Close the inspector';
    closeButton.textContent = '×';
    bar.append(title, detachButton, closeButton);
    const notice = page.createElement('div');
    notice.className = 'webfront-inspector-notice';
    notice.setAttribute('role', 'status');
    notice.hidden = true;
    const noticeText = page.createElement('span');
    const retry = page.createElement('button');
    retry.type = 'button';
    retry.textContent = 'Try again';
    notice.append(noticeText, retry);
    // A frame is made for each panel shown, its address set before it enters the page: navigating a frame that is already
    // in the page adds an entry to the window's history, and a window with a history may no longer close itself.
    let frame = page.createElement('iframe');
    frame.className = 'webfront-inspector-frame';
    frame.title = 'Inspector';
    panel.append(bar, notice, frame);
    page.body.append(panel);

    let shownPanel = null, retryPanel = null;
    // Each action on the floating window (show, hide, attach, detach) takes a new number: a detached container that answers
    // late only changes the window when no other action came meanwhile, so it never hides a panel shown since, nor shows
    // one closed since.
    let action = 0;
    const shown = () => clampRect(preferences.rect ?? defaultInspectorRect(viewport()), viewport());
    function render() {
        if (panel.hidden) return;
        const rect = shown();
        Object.assign(panel.style, {left: `${rect.x}px`, top: `${rect.y}px`, width: `${rect.width}px`, height: `${rect.height}px`});
    }
    function choose(rect) {
        preferences.rect = clampRect(rect, viewport());
        store.place(preferences.rect);
        render();
    }
    windowControls({view, bar, panel, shown, choose, viewport, shownNow: () => !panel.hidden});
    view.addEventListener('resize', render);

    const reason = () => detachedContainer(containers, {embedded: isEmbedded}) ? '' : hostReasons.noDetached;
    function say(text, again = null) {
        noticeText.textContent = text;
        retryPanel = again;
        retry.hidden = !again;
        notice.hidden = !text;
    }
    function showFloating(id, message = '') {
        ++action;
        shownPanel = id;
        title.textContent = panelTitle(id);
        panel.setAttribute('aria-label', `WebFront inspector: ${panelTitle(id)}`);
        const why = reason();
        detachButton.setAttribute('aria-disabled', String(Boolean(why)));
        // Greyed, it keeps its name and its tooltip says why it cannot detach.
        detachButton.title = why || inspectorMoves.detach;
        // The frame opens no connection of its own: the page reads ?frame and borrows this page's bridge.
        const url = urlOf(id, {frame: '1'});
        if (frame.getAttribute('src') !== url) {
            const next = page.createElement('iframe');
            next.className = 'webfront-inspector-frame';
            next.title = `Inspector: ${panelTitle(id)}`;
            next.setAttribute('src', url);
            frame.replaceWith(next);
            frame = next;
        }
        say(message, message && retryPanel);
        panel.hidden = false;
        render();
    }
    function hideFloating() {
        ++action;
        panel.hidden = true;
        shownPanel = null;
        const empty = page.createElement('iframe');  // unloads the panel without navigating
        empty.className = 'webfront-inspector-frame';
        empty.title = 'Inspector';
        frame.replaceWith(empty);
        frame = empty;
    }

    /**
     * Opens `id` in its detached container; resolves with whether it did. A failure leaves it in the floating window. A native
     * window that is no longer wanted once C++ has answered every opening of the panel (attached again, or its floating
     * window closed meanwhile) is closed then: the panel stays in the floating window, or closed.
     */
    async function openDetached(id, container) {
        // The relay keeps a bounded number of pages: one that lost its place announces itself again when its panel is detached.
        if (id === 'typescript.bridge' && containers?.host) publisher.announce();
        if (container === 'window') {
            const rect = shown(), started = action;
            const attempt = {cancelled: false};
            if (!opening.has(id)) opening.set(id, {pending: new Set(), latest: null});
            const openings = opening.get(id);
            openings.pending.add(attempt);
            openings.latest = attempt;
            let failure = null;
            try {
                // A close that an attach asked for is over first: the window opened now is never the one being closed.
                if (closing.has(id)) await closing.get(id);
                await bridge.cppFunction('webfront.inspector.openWindow')(id, self, rect.width, rect.height);
            } catch (error) {
                failure = error;
            }
            // A panel has one native window per host page, which every pending opening of it may reuse: whether it stays is
            // decided once the last of them has answered, whatever their order, by the latest one asked for.
            openings.pending.delete(attempt);
            if (!openings.pending.size) {
                opening.delete(id);
                if (!preferences.detached.has(id) || openings.latest.cancelled)
                    await bridge.cppFunction('webfront.inspector.closeWindow')(id, self).catch(() => {});
            }
            if (failure) {
                // Shown as the fallback only if nothing was done with the window while the native one was being opened.
                if (action === started) {
                    retryPanel = id;
                    showFloating(id, `${hostReasons.windowFailed}: ${failure?.message ?? failure}`);
                }
                return false;
            }
            return preferences.detached.has(id) && !attempt.cancelled;
        }
        // The target names the host too: a tab of another page of the same browsing group is never reused for this one. A tab
        // that was asked to close itself may not be closed yet: a new name opens another tab instead of reusing that one.
        const generation = selfClosing.get(id);
        const name = `webfront-inspector-${self}-${id.replace('.', '-')}${generation ? `-${generation}` : ''}`;
        const tab = openTab(urlOf(id), name);
        if (tab) {
            tabs.set(id, tab);
            return true;
        }
        retryPanel = id;
        showFloating(id, hostReasons.blocked);
        return false;
    }

    // The detached container of a panel attached again is closed by this page, ordered with its later detaches, and not by
    // the detached window once it got the answer: a detach between that answer and the window's close would reuse the
    // window, then lose it. `closing` holds the close of a native window still in progress, `tabs` the tabs this page opened,
    // and `selfClosing` the generation of the panels whose tab closes itself (a tab this page did not open, before a reload).
    const closing = new Map(), tabs = new Map(), selfClosing = new Map();
    let generation = 0;
    // The openings of a native window that C++ has not answered yet, per panel (Detach clicked twice makes two), and the
    // latest one asked for: closing the floating window that shows that panel meanwhile cancels all of them, and the
    // window is closed once the last of them has answered, unless a later detach asked for it again.
    const opening = new Map();
    /** Closes the floating window at the user's request, with the native windows still opening for its panel. */
    function closeFloating() {
        for (const attempt of (shownPanel && opening.get(shownPanel)?.pending) || []) attempt.cancelled = true;
        hideFloating();
    }
    /**
     * Closes the detached container of `id`; resolves with 'host' when this page closed it, 'self' when the window must close
     * itself. A native window counts as closed only when C++ says so: a close that failed or found no window answers 'self',
     * so that the window does not stay open waiting for a close that will not come.
     */
    async function closeDetached(id) {
        if (detachedContainer(containers, {embedded: isEmbedded}) === 'window') {
            const closed = bridge.cppFunction('webfront.inspector.closeWindow')(id, self).then(Boolean, () => false);
            closing.set(id, closed);
            closed.then(() => { if (closing.get(id) === closed) closing.delete(id); });
            return await closed ? 'host' : 'self';
        }
        const tab = tabs.get(id);
        tabs.delete(id);
        if (tab && !tab.closed) {
            tab.close();
            return 'host';
        }
        selfClosing.set(id, ++generation);
        return 'self';
    }

    /** Hides the floating window's `id` now that its detached container opened, unless it was attached or changed since. */
    function hideIfStill(id, started) {
        if (shownPanel === id && preferences.detached.has(id) && action === started) hideFloating();
    }

    // The state of the bridge in the page (#684): of the interface when it has a bridge (`inspected`), else of this page. It is
    // followed only while a panel reads, and published to C++ for the detached panels of an admitted page.
    const bridgeState = createBridgeMonitor({target: () => inspected?.() ?? {bridge, label: 'this page', path: view.location?.pathname ?? ''},
        timers: view, clock: () => view.performance?.now() ?? Date.now()});
    const publisher = createBridgePublisher({monitor: bridgeState, bridge, host: self, view});
    if (containers?.host) publisher.announce();

    const host = {
        get containers() { return containers; },
        get embedded() { return isEmbedded; },
        /** The identifier that names this page in its detached windows. */
        get id() { return self; },
        /** The bridge this host was created with: the frame of the floating window borrows it (inspector.mjs, pageBridge). */
        get bridge() { return bridge; },
        /** The monitor of the bridge in this page (inspector-bridge.mjs): the frame reads it, the detached panels read what it publishes. */
        get bridgeState() { return bridgeState; },
        /** Whether this page publishes the state of its bridge now: a detached Bridge panel reads it. */
        get bridgePublishing() { return publisher.running; },
        get shownPanel() { return shownPanel; },
        get frame() { return frame; },
        panel, bar, detachButton, closeButton, notice,
        reason,
        /** Shows `id` where the project keeps it: its detached container, or this page's floating window. */
        async open(id) {
            if (!panels.has(id)) throw new Error(`Unknown inspector panel ${id}`);
            const container = containerFor(id, {containers, embedded: isEmbedded, detached: preferences.detached});
            if (!container) return null;
            if (container === 'floating') {
                retryPanel = null;
                showFloating(id);
                return 'floating';
            }
            const started = action;
            if (await openDetached(id, container)) {
                hideIfStill(id, started);
                return container;
            }
            return 'floating';
        },
        /** Moves `id` to its detached container, and remembers it there. */
        async detach(id) {
            const container = detachedContainer(containers, {embedded: isEmbedded});
            if (!container) return null;
            preferences.detached.add(id);
            store.detached(id, true);
            const started = action;
            if (await openDetached(id, container)) {
                hideIfStill(id, started);
                return container;
            }
            return 'floating';
        },
        /** Moves `id` back to this page's floating window, and remembers it there. */
        attach(id) {
            preferences.detached.delete(id);
            store.detached(id, false);
            retryPanel = null;
            showFloating(id);
        },
        close: closeFloating,
        saved: () => store.saved(),
        get preferences() { return {detached: new Set(preferences.detached), rect: preferences.rect}; },
    };

    detachButton.addEventListener('click', () => {
        if (shownPanel && !reason()) host.detach(shownPanel);
    });
    closeButton.addEventListener('click', closeFloating);
    retry.addEventListener('click', () => { if (retryPanel) host.detach(retryPanel); });

    // A detached inspector asks to come back: the page that detached it shows the panel, closes the detached container when
    // it can, and answers who closes it ('host', or 'self' for a tab this page did not open).
    if (typeof view.BroadcastChannel === 'function') {
        const channel = new view.BroadcastChannel(inspectorChannel);
        channel.addEventListener('message', ({data}) => {
            // Only the page that detached the panel takes it back: other pages of the same origin ignore the request.
            if (data?.type !== 'attach' || data.host !== self || !panels.has(data.panel) || !containers?.host) return;
            host.attach(data.panel);
            closeDetached(data.panel).then(closer => channel.postMessage({type: 'attached', id: data.id, closer}));
        });
        host.channel = channel;
    }
    // A place still waiting for its write is written when the page goes away (a detach is written at once).
    view.addEventListener('pagehide', () => { store.saved().catch(() => {}); });
    // The frame of the floating window reaches its host here: the same origin, and no connection of its own.
    view.webfrontInspectorHost = host;
    return host;
}

/**
 * The inspectors of a page that holds the tab: `open(panel)` shows a panel, creating the host on first use. The host is also
 * created as soon as `access` says that this page reaches the inspectors, without waiting for a panel to be opened: a panel
 * detached before this page was reloaded carries the page's identifier, and its request to be attached again must find the
 * host listening. `host()` is the promise of the host, null until it is created; a creation that failed is tried again on the
 * next `open(panel)`. `create` replaces createInspectorHost (tests).
 */
export function prepareInspectors({create = createInspectorHost, ...options}) {
    let made = null;
    const host = () => made ??= Promise.resolve().then(() => create(options)).catch(error => {
        made = null;
        throw error;
    });
    Promise.resolve(options.access).then(access => { if (access?.containers?.host) host().catch(() => {}); }, () => {});
    return {
        open: panel => host().then(created => created.open(panel)),
        host: () => made,
    };
}
