// The context menu of the WebFront tab (#679, ADR-027): the terminal and the inspectors of the application's state. A
// click on the tab and the shortcut keep opening and collapsing the terminal; the menu opens with a right click, a long
// press on a touch screen, the Menu key or Shift+F10 on the tab. While it is closed it takes no key and no pointer from
// the interface: its listeners are on the tab, and on the page only while it is open.

// --- Entries ---------------------------------------------------------------------------------------------------

/** The inspector panels, by submenu; `id` is what the environment's inspector opens. */
export const inspectorMenus = [
    {id: 'cpp', label: 'C++ state', children: [
        {id: 'cpp.functions', label: 'Functions'},
        {id: 'cpp.browsers', label: 'Browsers and tasks'},
        {id: 'cpp.watches', label: 'Watches'},
    ]},
    {id: 'typescript', label: 'TypeScript state', children: [
        {id: 'typescript.packages', label: 'Packages'},
        {id: 'typescript.bridge', label: 'Bridge'},
    ]},
];

// --- Access to the inspectors ----------------------------------------------------------------------------------

/** The reserved function that says which containers would reach the inspectors; the only one that checks no access. */
export const containersFunction = 'webfront.inspector.containers';

export const accessReasons = {
    checking: 'Checking whether this page may open the inspectors…',
    tooOld: 'The WebFront library of this application has no inspectors (webfront.inspector.*): update it.',
    noContainer: 'The inspectors answer only a view of the application, and this page is not one: use an embedded frontend '
        + '(WebKitGTK, WKWebView, WebView2 or CEF), or set WebFrontConfig::trustLoopbackClients, knowing that any local '
        + 'process can then read the state of the application.',
    noInspector: 'This version of the developer environment cannot open the inspectors yet.',
};

/**
 * What the menu can offer from the answer of webfront.inspector.containers(), a tuple of three booleans
 * (secondaryWindow, host, newConnections; ADR-027, section 8), or from the error of that call. `containers` is null when
 * no container reaches the inspectors, and `reason` then says why.
 *
 * Every container needs the host page to be admitted: a secondary window is opened by an admitted page, the floating
 * window borrows its bridge, and a tab of the system browser is admitted only by trustLoopbackClients, which admits a
 * loopback page too. A page that is not admitted while new connections would be is not on the loopback, and neither
 * would its tab be.
 */
export function inspectorAccess(answer, error = null) {
    if (error) {
        if (error?.code === 'webfront.notFound') return {containers: null, reason: accessReasons.tooOld};
        return {containers: null, reason: `The inspectors cannot be reached: ${error?.message ?? error}`};
    }
    if (!Array.isArray(answer) || answer.length !== 3 || !answer.every(value => typeof value === 'boolean'))
        return {containers: null, reason: 'The inspectors gave an answer that this environment does not read.'};
    const [secondaryWindow, host, newConnections] = answer;
    if (!host) return {containers: null, reason: accessReasons.noContainer};
    return {containers: {secondaryWindow, host, newConnections}, reason: ''};
}

/** Asks C++ once which containers would reach the inspectors. */
export async function readInspectorAccess(bridge) {
    try { return inspectorAccess(await bridge.cppFunction(containersFunction)()); }
    catch (error) { return inspectorAccess(null, error ?? new Error('unknown error')); }
}

// --- Geometry --------------------------------------------------------------------------------------------------

/**
 * Where a menu of `size` opens beside `anchor` (a rectangle {left, top, right, bottom}; a point has left === right and
 * top === bottom), inside the viewport: on the left of the anchor, since the tab is against the right edge, or on its
 * right when the left has no room; level with the anchor's top, or ending at its bottom when the window has no room
 * below. A menu larger than the viewport starts at its edge (the page scrolls the menu).
 */
export function placeMenu(anchor, size, viewport) {
    const fitsLeft = anchor.left - size.width >= 0, fitsRight = anchor.right + size.width <= viewport.width;
    const side = fitsLeft || !fitsRight ? 'left' : 'right';
    const x = side === 'left' ? anchor.left - size.width : anchor.right;
    const y = anchor.top + size.height <= viewport.height ? anchor.top : anchor.bottom - size.height;
    const within = (value, extent, room) => Math.round(Math.max(0, Math.min(value, room - extent)));
    return {x: within(x, size.width, viewport.width), y: within(y, size.height, viewport.height), side};
}

/** How long a touch stays on the tab before the menu opens instead of the terminal, and how far it may move. */
export const longPress = {delay: 550, distance: 10};

// --- Menu ------------------------------------------------------------------------------------------------------

export const menuStyles = `
.webfront-menu{position:fixed;z-index:2147483647;box-sizing:border-box;min-width:220px;max-width:min(360px,100vw);max-height:100vh;overflow:auto;padding:4px;background:#202737;color:#e8edf4;border:1px solid #46536a;border-radius:8px;box-shadow:0 8px 32px #000a;font:14px system-ui}
.webfront-menu[hidden]{display:none}
.webfront-menu [role=menu]{display:flex;flex-direction:column}
.webfront-menu-item{box-sizing:border-box;display:flex;align-items:center;gap:16px;width:100%;min-height:44px;margin:0;padding:0 12px;border:0;border-radius:4px;background:none;color:inherit;font:inherit;text-align:start;cursor:pointer}
.webfront-menu-item>span:first-child{flex:1 1 auto}
.webfront-menu-item:focus{outline:none}
.webfront-menu-item:hover,.webfront-menu-item:focus-visible,.webfront-menu-item[aria-expanded=true]{background:#2f3a50}
.webfront-menu-item:focus-visible{outline:2px solid #d7fa85;outline-offset:-2px}
.webfront-menu-item[aria-disabled=true]{color:#8d97aa;cursor:default}
.webfront-menu-hint{color:#a9b3c4;font-size:12px}
.webfront-menu-reason{margin:4px 0 0;padding:8px 12px;border-top:1px solid #46536a;color:#c8cfdb;font-size:12px;line-height:1.4}
.webfront-menu-reason:empty{display:none}
`;

let menus = 0;

/**
 * The menu of the tab `button`. `entries` are {id, label, hint?, children?, reason?()}: an entry with children opens a
 * submenu, the others call `activate(id)`; `reason()` gives why an entry is unavailable ('' when it is available), read
 * each time the menu or the submenu opens, and again by `menu.refresh()` when what it depends on changes while a panel
 * is open. `menu.consumeClick()` tells the tab's click handler that a long press opened
 * the menu, so that the click that ends it does not also toggle the terminal.
 */
export function createTabMenu({button, entries, activate}) {
    const page = button.ownerDocument, view = page.defaultView;
    const number = ++menus;
    const levels = [];         // the open panels, the root first: {panel, list, items, entries, parent}
    let longTimer = null, pressed = null, swallowClick = false, lastPointer = null;

    const reasonOf = entry => (entry.reason?.() ?? '') || '';

    function panelFor(list, parentItem, depth) {
        const panel = page.createElement('div');
        panel.className = 'webfront-menu';
        panel.dataset.level = String(depth);
        const menu = page.createElement('div');
        menu.setAttribute('role', 'menu');
        menu.id = `webfront-menu-${number}-${depth}`;
        if (parentItem) menu.setAttribute('aria-labelledby', parentItem.id);
        else menu.setAttribute('aria-label', 'WebFront');
        const note = page.createElement('p');
        note.className = 'webfront-menu-reason';
        note.id = `${menu.id}-reason`;
        const reasons = [];
        const items = list.map((entry, index) => {
            const item = page.createElement('button');
            item.type = 'button';
            item.tabIndex = -1;
            item.className = 'webfront-menu-item';
            item.id = `${menu.id}-${index}`;
            item.setAttribute('role', 'menuitem');
            item.dataset.entry = entry.id;
            const label = page.createElement('span');
            label.textContent = entry.label;
            item.append(label);
            if (entry.children) {
                item.setAttribute('aria-haspopup', 'menu');
                item.setAttribute('aria-expanded', 'false');
                const arrow = page.createElement('span');
                arrow.setAttribute('aria-hidden', 'true');
                arrow.textContent = '▸';
                item.append(arrow);
            } else if (entry.hint) {
                const hint = page.createElement('span');
                hint.className = 'webfront-menu-hint';
                hint.textContent = entry.hint;
                item.append(hint);
                if (entry.keys) item.setAttribute('aria-keyshortcuts', entry.keys);
            }
            return item;
        });
        menu.append(...items);
        panel.append(menu, note);
        const level = {panel, menu, note, items, entries: list, parent: parentItem};
        applyReasons(level);
        return level;
    }

    /** Greys the entries of a panel that are unavailable, with their reason, and enables the others. */
    function applyReasons({note, items, entries: list}) {
        const reasons = [];
        list.forEach((entry, index) => {
            const item = items[index], reason = entry.children ? '' : reasonOf(entry);
            if (reason) {
                item.setAttribute('aria-disabled', 'true');
                item.setAttribute('aria-describedby', note.id);
                item.title = reason;
                if (!reasons.includes(reason)) reasons.push(reason);
            } else {
                item.removeAttribute('aria-disabled');
                item.removeAttribute('aria-describedby');
                item.removeAttribute('title');
            }
        });
        note.textContent = reasons.join(' ');
    }

    /** Places a panel beside its anchor (level.anchor()), inside the viewport, for the size it has now. */
    function place(level) {
        const {panel} = level, anchor = level.anchor();
        panel.style.left = '0px';
        panel.style.top = '0px';
        const viewport = {width: page.documentElement.clientWidth, height: page.documentElement.clientHeight};
        const {x, y, side} = placeMenu(anchor, {width: panel.offsetWidth, height: panel.offsetHeight}, viewport);
        panel.style.left = `${x}px`;
        panel.style.top = `${y}px`;
        panel.dataset.side = side;
    }

    const isOpen = () => levels.length > 0;
    const levelOf = element => levels.findIndex(level => level.panel.contains(element));

    function closeFrom(depth) {
        while (levels.length > depth) {
            const level = levels.pop();
            level.panel.remove();
            level.parent?.setAttribute('aria-expanded', 'false');
        }
    }

    function close({focus = true} = {}) {
        if (!isOpen()) return;
        closeFrom(0);
        page.removeEventListener('pointerdown', outside, true);
        page.removeEventListener('focusin', focusMoved, true);
        view.removeEventListener('blur', windowLeft);
        view.removeEventListener('resize', closeQuietly);
        if (focus) button.focus();
    }
    const closeQuietly = () => close({focus: false});

    /** Opens the menu beside `anchor` (the tab's rectangle by default) with the focus on its first entry. */
    function open(anchor = button.getBoundingClientRect()) {
        if (isOpen()) return;
        const root = panelFor(entries, null, 0);
        levels.push(root);
        page.body.append(root.panel);
        root.anchor = () => anchor;
        place(root);
        root.panel.addEventListener('keydown', onKey);
        root.panel.addEventListener('click', onClick);
        root.panel.addEventListener('pointerover', onHover);
        page.addEventListener('pointerdown', outside, true);
        page.addEventListener('focusin', focusMoved, true);
        view.addEventListener('blur', windowLeft);
        view.addEventListener('resize', closeQuietly);
        root.items[0]?.focus();
    }

    function openSubmenu(item, {focus = true} = {}) {
        const depth = levelOf(item), level = levels[depth];
        const entry = level.entries[level.items.indexOf(item)];
        if (!entry?.children) return;
        if (levels[depth + 1]?.parent === item) {
            if (focus) levels[depth + 1].items[0]?.focus();
            return;
        }
        closeFrom(depth + 1);
        const sub = panelFor(entry.children, item, depth + 1);
        levels.push(sub);
        item.setAttribute('aria-controls', sub.menu.id);
        item.setAttribute('aria-expanded', 'true');
        page.body.append(sub.panel);
        // Beside its parent panel, level with its entry: read again each time it is placed, since the parent may move.
        sub.anchor = () => {
            const rect = item.getBoundingClientRect(), parentRect = level.panel.getBoundingClientRect();
            return {left: parentRect.left, right: parentRect.right, top: rect.top, bottom: rect.bottom};
        };
        place(sub);
        sub.panel.addEventListener('keydown', onKey);
        sub.panel.addEventListener('click', onClick);
        sub.panel.addEventListener('pointerover', onHover);
        if (focus) sub.items[0]?.focus();
    }

    function choose(item) {
        const level = levels[levelOf(item)];
        const entry = level?.entries[level.items.indexOf(item)];
        if (!entry) return;
        if (entry.children) { openSubmenu(item); return; }
        if (item.getAttribute('aria-disabled') === 'true') return;  // its reason stays on screen
        close({focus: false});
        activate(entry.id);
    }

    function onKey(event) {
        const item = event.target.closest?.('.webfront-menu-item');
        const depth = levelOf(event.target);
        if (depth < 0) return;
        const {items} = levels[depth];
        const index = items.indexOf(item);
        const move = to => items[(to + items.length) % items.length]?.focus();
        let handled = true;
        switch (event.key) {
        case 'ArrowDown': move(index + 1); break;
        case 'ArrowUp': move(index < 0 ? -1 : index - 1); break;
        case 'Home': move(0); break;
        case 'End': move(-1); break;
        case 'ArrowRight':
            if (item?.hasAttribute('aria-haspopup')) openSubmenu(item);
            break;
        case 'ArrowLeft':
            if (depth > 0) {
                const parent = levels[depth].parent;
                closeFrom(depth);
                parent.focus();
            }
            break;
        case 'Escape':
            if (depth > 0) {
                const parent = levels[depth].parent;
                closeFrom(depth);
                parent.focus();
            } else close();
            break;
        case 'Enter': case ' ':
            if (item) choose(item);
            break;
        case 'Tab':
            // The menu closes and the focus goes back to the tab before the browser moves it: the default action is kept,
            // so Tab goes on to the element after the tab and Shift+Tab to the one before, as from the tab itself.
            close();
            event.stopPropagation();
            return;
        default: handled = false;
        }
        if (!handled) return;
        event.preventDefault();
        event.stopPropagation();
    }

    function onClick(event) {
        const item = event.target.closest?.('.webfront-menu-item');
        if (item) choose(item);
    }

    // A pointer that rests on an entry with a submenu opens it, and one on another entry of that menu closes it.
    function onHover(event) {
        if (event.pointerType === 'touch') return;
        const item = event.target.closest?.('.webfront-menu-item');
        if (!item) return;
        const depth = levelOf(item);
        if (item.hasAttribute('aria-haspopup')) openSubmenu(item, {focus: false});
        else closeFrom(depth + 1);
        item.focus();
    }

    // A press anywhere else closes it, on the tab too: the click that follows toggles the terminal as usual.
    function outside(event) {
        if (levelOf(event.target) < 0) closeQuietly();
    }
    // The focus left the menu for another element of the page, or for the interface's frame (the window loses it).
    function focusMoved(event) {
        if (levelOf(event.target) < 0) closeQuietly();
    }
    function windowLeft() { closeQuietly(); }

    // --- Opening ---
    button.setAttribute('aria-haspopup', 'menu');
    button.style.setProperty('-webkit-touch-callout', 'none');
    button.addEventListener('contextmenu', event => {
        event.preventDefault();
        if (isOpen()) return;  // already opened by the long press or the key that caused this event
        // A right click opens the menu where it is; the Menu key and Shift+F10 beside the tab.
        const atPointer = lastPointer && view.performance.now() - lastPointer.time < 1000;
        open(atPointer ? {left: lastPointer.x, right: lastPointer.x, top: lastPointer.y, bottom: lastPointer.y} : undefined);
    });
    button.addEventListener('keydown', event => {
        const menuKey = event.key === 'ContextMenu' || (event.key === 'F10' && event.shiftKey && !event.ctrlKey && !event.altKey && !event.metaKey);
        if (!menuKey) return;
        event.preventDefault();
        event.stopPropagation();
        open();
    });
    const cancelPress = () => {
        view.clearTimeout(longTimer);
        longTimer = null;
        pressed = null;
    };
    button.addEventListener('pointerdown', event => {
        swallowClick = false;
        if (event.pointerType === 'mouse' && event.button === 2 || event.pointerType !== 'mouse')
            lastPointer = {x: event.clientX, y: event.clientY, time: view.performance.now()};
        if (event.pointerType === 'mouse') return;
        cancelPress();
        pressed = {x: event.clientX, y: event.clientY};
        longTimer = view.setTimeout(() => {
            longTimer = null;
            swallowClick = true;
            open({left: pressed.x, right: pressed.x, top: pressed.y, bottom: pressed.y});
        }, longPress.delay);
    });
    button.addEventListener('pointermove', event => {
        if (pressed && Math.hypot(event.clientX - pressed.x, event.clientY - pressed.y) > longPress.distance) cancelPress();
    });
    button.addEventListener('pointerup', () => { view.clearTimeout(longTimer); longTimer = null; });
    button.addEventListener('pointercancel', cancelPress);

    return {
        open, close,
        /**
         * Reads the reasons again for the panels that are open, which keep their focus. A reason may change their size:
         * each one is placed again beside its anchor, the root first, so that a submenu follows a parent that moved.
         */
        refresh() {
            levels.forEach(applyReasons);
            levels.forEach(place);
        },
        get isOpen() { return isOpen(); },
        /** The panels shown, the root first. */
        get panels() { return levels.map(level => level.panel); },
        /** Whether the click that follows a long press must be ignored; reading it clears it. */
        consumeClick() {
            const swallowed = swallowClick;
            swallowClick = false;
            return swallowed;
        },
    };
}
