// The layout of the developer page: the terminal alone while the project has no interface, then the interface over the
// whole window with the terminal in a floating, movable window that an icon opens and collapses. The terminal is mounted
// once in its window and never moves in the document afterwards: collapsing only hides it, so the session, a running
// command, the line being typed and the scrollback are kept. The interface stays in the preview frame.
import {call, readProject, publish} from './io.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// --- Existing interface ----------------------------------------------------------------------------------------

/** Where wf dev and wf preview publish the interface; a publication replaces it whole, so a failed build keeps the last one. */
export const previewDirectory = '.webfront/developer/preview';

/** Whether the project already has an interface to show: a published preview with its page. */
export async function hasInterface(bridge) {
    try { return JSON.parse(decoder.decode(await call(bridge, 'list', previewDirectory))).includes('index.html'); }
    catch { return false; }  // no preview directory yet
}

// --- States ----------------------------------------------------------------------------------------------------

/** terminal: no interface, the terminal fills the window; interface: the interface alone; floating: the terminal over it. */
export const State = {terminal: 'terminal', interface: 'interface', floating: 'floating'};

/** The state a page opens in. */
export const initialState = interfaceExists => interfaceExists ? State.interface : State.terminal;

const transitions = {
    // An interface that appears while the terminal is in use leaves it open, over the interface.
    [State.terminal]: {shown: State.floating},
    [State.interface]: {toggle: State.floating, open: State.floating, hidden: State.terminal},
    [State.floating]: {toggle: State.interface, close: State.interface, hidden: State.terminal},
};

/**
 * The state after an event: `shown` (a published preview is loaded in the frame), `hidden` (the frame is hidden), `toggle`
 * (the icon or the shortcut), `open` and `close`. An event that does not apply leaves the state unchanged.
 */
export const nextState = (state, event) => transitions[state]?.[event] ?? state;

// --- Geometry --------------------------------------------------------------------------------------------------

export const minimumSize = {width: 320, height: 160};
export const step = 16;
const clamp = (value, low, high) => Math.min(Math.max(value, low), Math.max(low, high));

/** The window fitted to the viewport: no larger than it (even below the minimum size) and entirely inside it. */
export function clampRect(rect, viewport) {
    const width = Math.round(Math.min(clamp(rect.width, minimumSize.width, viewport.width), viewport.width));
    const height = Math.round(Math.min(clamp(rect.height, minimumSize.height, viewport.height), viewport.height));
    return {x: Math.round(clamp(rect.x, 0, viewport.width - width)), y: Math.round(clamp(rect.y, 0, viewport.height - height)), width, height};
}

/** Where the window opens when the project has no remembered place: at the bottom of the viewport, clear of the icon. */
export function defaultRect(viewport) {
    const width = Math.min(760, viewport.width - 2 * step), height = Math.min(360, Math.round(viewport.height * 0.45));
    return clampRect({x: viewport.width - width - step, y: viewport.height - height - step, width, height}, viewport);
}

const arrows = {ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1]};

/** The window after an arrow key on its title bar: moved by a step, or resized with Shift. Null for another key. */
export function keyboardRect(rect, {key, shiftKey = false}, viewport) {
    if (!Object.hasOwn(arrows, key)) return null;
    const [dx, dy] = arrows[key].map(direction => direction * step);
    return clampRect(shiftKey ? {...rect, width: rect.width + dx, height: rect.height + dy} : {...rect, x: rect.x + dx, y: rect.y + dy}, viewport);
}

/** F9 alone: neither the line editor nor xterm.js uses it, and browsers leave it to the page. */
export const isToggleKey = event => event.key === 'F9' && !(event.ctrlKey || event.altKey || event.metaKey || event.shiftKey);

// --- Remembered place ------------------------------------------------------------------------------------------

/** The place of the window, kept in the project like the history: each project has its own. */
export const placePath = '.webfront/developer/terminal-window.json';

/** The remembered rectangle of a file's text; null when it is not one. */
export function parsePlace(text) {
    try {
        const {format, x, y, width, height} = JSON.parse(text);
        const rect = {x, y, width, height};
        if (format !== 1 || !Object.values(rect).every(value => Number.isFinite(value) && Math.abs(value) < 1e6)) return null;
        return rect.width > 0 && rect.height > 0 ? rect : null;
    } catch { return null; }
}

export const formatPlace = ({x, y, width, height}) => JSON.stringify({format: 1, x, y, width, height}) + '\n';

export async function readPlace(bridge) {
    try { return parsePlace(decoder.decode(await readProject(bridge, placePath))); }
    catch { return null; }
}

/**
 * Remembers the last place given, shortly after it stops changing; a failed write keeps the window usable. `saved()`
 * resolves once the places given so far are written.
 */
export function placeStore(bridge, delay = 300) {
    let timer = null, pending = null, writes = Promise.resolve(), written = null;
    const flush = () => {
        clearTimeout(timer);
        timer = null;
        if (!pending) return writes;
        const text = formatPlace(pending);
        pending = null;
        // Compared once the writes before this one are done: a place chosen again while another one is being written
        // must still replace it.
        writes = writes.then(async () => {
            if (text === written) return;
            await publish(bridge, placePath, new Map([['output', encoder.encode(text)]]), {directory: false});
            written = text;
        }).catch(() => {});
        return writes;
    };
    return {
        remember(rect) {
            pending = rect;
            clearTimeout(timer);
            timer = setTimeout(flush, delay);
        },
        saved: () => flush(),
    };
}

// --- Page ------------------------------------------------------------------------------------------------------

// The WebFront swallow, shipped in the environment.
const logo = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="28" height="28" aria-hidden="true" focusable="false">'
    + '<g transform="translate(1 14) scale(.089)"><g transform="translate(-40 -150)">'
    + '<path fill="#203b30" d="M236 168C378 177 488 216 565 296C609 267 645 274 679 296L710 309C672 307 650 327 630 359C578 451 442 514 295 541C367 485 444 416 476 385C488 372 447 373 406 380C274 406 172 456 63 493L253 381C244 365 164 340 60 320C237 342 377 355 433 320C449 308 364 237 236 168Z"/>'
    + '<path fill="#ffffff" d="M540 294C594 302 621 349 597 393C590 405 581 414 568 423C592 379 585 327 540 294Z"/>'
    + '<path fill="#d7fa85" d="M599 333C620 310 646 299 674 305C646 318 643 342 630 363C613 389 589 409 568 423C593 391 603 361 599 333Z"/>'
    + '<path fill="#d7fa85" d="M132 336L322 373L135 465L285 375Z"/></g></g></svg>';

const attribute = 'data-webfront-workspace';
const floating = `:root:is([${attribute}=interface],[${attribute}=floating])`;
const styles = `
:root[${attribute}],:root[${attribute}] body{height:100%}
:root[${attribute}] body{display:flex;flex-direction:column;overflow:hidden}
${floating} body>header{display:none}
.webfront-terminal-window{display:flex;flex-direction:column;flex:1 1 auto;min-height:0;box-sizing:border-box;background:#141821}
${floating} .webfront-terminal-window{position:fixed;z-index:2147483646;border:1px solid #46536a;border-radius:8px;box-shadow:0 8px 32px #000a;resize:both;overflow:hidden;max-width:100vw;max-height:100vh}
:root[${attribute}=interface] .webfront-terminal-window{visibility:hidden}
.webfront-terminal-bar{display:none;flex:none;align-items:center;justify-content:space-between;padding:0 4px 0 12px;background:#202737;color:#e8edf4;font:13px system-ui;cursor:move;user-select:none;touch-action:none}
${floating} .webfront-terminal-bar{display:flex}
.webfront-terminal-bar:focus-visible,.webfront-terminal-close:focus-visible,.webfront-terminal-toggle:focus-visible{outline:2px solid #d7fa85;outline-offset:-2px}
.webfront-terminal-close{margin:0;padding:0;width:32px;height:32px;border:0;background:none;color:inherit;font:20px/1 system-ui;cursor:pointer}
.webfront-terminal-toggle{position:fixed;top:8px;right:8px;z-index:2147483647;margin:0;padding:0;width:44px;height:44px;display:grid;place-items:center;border:1px solid #46536a;border-radius:50%;background:#f6f7f2;box-shadow:0 2px 8px #0006;cursor:pointer;opacity:.9}
.webfront-terminal-toggle:hover,.webfront-terminal-toggle[aria-expanded=true]{opacity:1}
:root[${attribute}=terminal] .webfront-terminal-toggle,:root[${attribute}=terminal] .webfront-interface{display:none}
`;

const managed = new WeakSet();
/** Whether a workspace already lays out this preview frame: a page has one, whatever the terminals mounted in it. */
export const isManaged = frame => managed.has(frame);

/**
 * Lays the page out around the terminal's `element` and the preview `frame`, which starts hidden. Call it before the
 * terminal opens in `element`, then attach({focus, fit}) with the functions that focus the terminal and fit it to `element`.
 * The result gives the `state`, `interfaceExists` (the page opened on an existing interface, to load in the frame),
 * toggle(), open({focus}) and close(), `button` and `panel`, and placeSaved().
 */
export async function createWorkspace({bridge, element, frame}) {
    managed.add(frame);
    const page = element.ownerDocument, view = page.defaultView, root = page.documentElement;
    const [interfaceExists, remembered] = await Promise.all([hasInterface(bridge), readPlace(bridge)]);
    const store = placeStore(bridge);
    const viewport = () => ({width: root.clientWidth, height: root.clientHeight});
    let state = initialState(interfaceExists);
    let place = remembered;  // as chosen; what is displayed is fitted to the viewport
    let focusTerminal = () => {}, fitTerminal = () => {};

    const style = page.createElement('style');
    style.textContent = styles;
    page.head.append(style);

    const panel = page.createElement('section');
    panel.className = 'webfront-terminal-window';
    panel.id = 'webfront-terminal-window';
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'false');
    panel.setAttribute('aria-label', 'WebFront terminal');
    const bar = page.createElement('div');
    bar.className = 'webfront-terminal-bar';
    bar.tabIndex = 0;
    bar.setAttribute('role', 'group');
    bar.setAttribute('aria-label', 'Terminal window: drag it, or use the arrow keys to move it and Shift with the arrow keys to resize it');
    const title = page.createElement('span');
    title.textContent = 'Terminal';
    const closeButton = page.createElement('button');
    closeButton.type = 'button';
    closeButton.className = 'webfront-terminal-close';
    closeButton.setAttribute('aria-label', 'Hide the terminal');
    closeButton.title = 'Hide the terminal (F9)';
    closeButton.textContent = '×';
    bar.append(title, closeButton);
    element.replaceWith(panel);
    panel.append(bar, element);
    Object.assign(element.style, {flex: '1 1 auto', height: 'auto', minHeight: '0', boxSizing: 'border-box'});
    Object.assign(frame.style, {position: 'fixed', inset: '0', width: '100%', height: '100%'});
    frame.classList.add('webfront-interface');

    const button = page.createElement('button');
    button.type = 'button';
    button.className = 'webfront-terminal-toggle';
    button.innerHTML = logo;
    button.setAttribute('aria-controls', panel.id);
    page.body.append(button);

    // The displayed rectangle; the chosen one is kept, so that a window enlarged again gives the terminal its place back.
    const shown = () => clampRect(place ?? defaultRect(viewport()), viewport());
    function render() {
        root.setAttribute(attribute, state);
        const open = state === State.floating;
        button.setAttribute('aria-expanded', String(open));
        button.setAttribute('aria-label', open ? 'Hide the WebFront terminal' : 'Show the WebFront terminal');
        button.title = `${open ? 'Hide' : 'Show'} the terminal (F9)`;
        const rect = state === State.terminal ? null : shown();
        Object.assign(panel.style, rect ? {left: `${rect.x}px`, top: `${rect.y}px`, width: `${rect.width}px`, height: `${rect.height}px`}
            : {left: '', top: '', width: '', height: ''});
        fitTerminal();  // its window may have changed size
    }
    function choose(rect) {
        place = clampRect(rect, viewport());
        store.remember(place);
        render();
    }
    function apply(event, {focus = true} = {}) {
        const before = state;
        state = nextState(state, event);
        if (state === before) return;
        render();
        if (!focus) return;
        if (state === State.interface) frame.focus();  // the keys go back to the interface
        else focusTerminal();
    }

    button.addEventListener('click', () => apply('toggle'));
    closeButton.addEventListener('click', () => apply('close'));
    // Only a published preview loaded in the frame is an interface. wf dev also unhides the frame to show the diagnostic
    // of a failed build: before any preview, the frame stays out of the page and the terminal, which prints the
    // diagnostic too, keeps the window.
    frame.addEventListener('webfront-preview-navigating', () => apply('shown'));
    new view.MutationObserver(() => { if (frame.hidden) apply('hidden'); }).observe(frame, {attributes: true, attributeFilter: ['hidden']});

    // The shortcut works wherever the focus is: in this page and in the interface, whose window changes at each load.
    const onKey = event => {
        if (!isToggleKey(event) || state === State.terminal) return;
        event.preventDefault();
        event.stopPropagation();
        apply('toggle');
    };
    view.addEventListener('keydown', onKey, true);
    const watchInterface = () => {
        try { frame.contentWindow?.addEventListener('keydown', onKey, true); }
        catch { /* a page of another origin keeps its keys */ }
    };
    frame.addEventListener('load', watchInterface);
    watchInterface();

    // Moving: the pointer is captured, so that the interface under the window does not take the moves.
    let grip = null;
    bar.addEventListener('pointerdown', event => {
        if (event.button !== 0 || event.target === closeButton) return;
        const rect = shown();
        grip = {x: event.clientX - rect.x, y: event.clientY - rect.y};
        try { bar.setPointerCapture(event.pointerId); }
        catch { /* a pointer that is already gone: the move ends with it */ }
        event.preventDefault();
        bar.focus();
    });
    bar.addEventListener('pointermove', event => {
        if (grip) choose({...shown(), x: event.clientX - grip.x, y: event.clientY - grip.y});
    });
    const release = () => { grip = null; };
    bar.addEventListener('pointerup', release);
    bar.addEventListener('pointercancel', release);
    bar.addEventListener('keydown', event => {
        if (event.target !== bar) return;
        const rect = keyboardRect(shown(), event, viewport());
        if (!rect) return;
        event.preventDefault();
        choose(rect);
    });
    // The corner handle (CSS resize) writes the size on the element: it becomes the chosen one.
    new view.ResizeObserver(() => {
        if (state !== State.floating) return;
        const rect = shown(), width = panel.offsetWidth, height = panel.offsetHeight;
        if (width && height && (width !== rect.width || height !== rect.height)) choose({...rect, width, height});
    }).observe(panel);
    view.addEventListener('resize', render);

    render();
    return {
        get state() { return state; },
        interfaceExists, button, panel, bar,
        attach({focus, fit = () => {}}) {
            focusTerminal = focus;
            fitTerminal = fit;
            if (state === State.interface) frame.focus(); else focus();
        },
        toggle: () => apply('toggle'),
        open: options => apply('open', options),
        close: () => apply('close'),
        placeSaved: () => store.saved(),
    };
}
