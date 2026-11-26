// Specs of the developer page's layout (workspace.mjs): what counts as an existing interface, the states of the display,
// and the place of the floating terminal window. The page itself is checked in test/web/developer-specs.mjs.
import {hasInterface, previewDirectory, State, initialState, nextState, clampRect, defaultRect, keyboardRect, minimumSize, step,
    windowControls, isToggleKey, Status, tabStatus, statusText, defaultShortcut, shortcutText, readShortcut, parsePlace, formatPlace, readPlace, placeStore, placePath,
    createWorkspace} from '../../../js/developer/environment/workspace.mjs';
import {publish} from '../../../js/developer/environment/io.mjs';
import {inspectorAccess, readInspectorAccess, accessReasons, containersFunction, placeMenu, createTabMenu, longPress, inspectorMenus,
    menuStyles} from '../../../js/developer/environment/menu.mjs';
import {memoryBridge} from '../memory-bridge.mjs';

describe('The developer page layout', () => {
    const encode = text => new TextEncoder().encode(text);
    const page = text => new Map([['index.html', encode(text)], ['app.js', encode('')]]);

    describe('window controls', () => {
        it('move the window by the pointer that started the move only', () => {
            const bar = document.createElement('div'), panel = document.createElement('div');
            document.body.append(bar, panel);
            try {
                let rect = {x: 100, y: 100, width: 400, height: 200};
                windowControls({view: window, bar, panel, shown: () => rect, choose: next => { rect = next; }, viewport: () => ({width: 2000, height: 2000}),
                    shownNow: () => false});
                const pointer = (type, pointerId, x, y) => bar.dispatchEvent(new PointerEvent(type, {bubbles: true, cancelable: true, pointerId, clientX: x, clientY: y, button: 0}));
                pointer('pointerdown', 1, 110, 110);
                pointer('pointerdown', 2, 300, 110);  // a second finger: ignored
                pointer('pointermove', 1, 160, 130);
                expect([rect.x, rect.y]).toEqual([150, 120]);
                pointer('pointermove', 2, 900, 900);  // the second finger does not move the window
                expect([rect.x, rect.y]).toEqual([150, 120]);
                pointer('pointerup', 2, 900, 900);    // nor does lifting it end the move
                pointer('pointermove', 1, 170, 140);
                expect([rect.x, rect.y]).toEqual([160, 130]);
                pointer('pointerup', 1, 170, 140);
                pointer('pointermove', 1, 500, 500);
                expect([rect.x, rect.y]).toEqual([160, 130]);
            } finally { bar.remove(); panel.remove(); }
        });
    });

    describe('existing interface', () => {
        it('is a published preview page, which wf create and wf build do not produce', async () => {
            const {bridge, storage} = memoryBridge();
            expect(await hasInterface(bridge)).toBeFalse();
            // wf create writes sources, wf build publishes beside the preview.
            storage.set('ui/main.tsx', encode('export {}'));
            await publish(bridge, '.webfront/developer/build', page('built'));
            expect(await hasInterface(bridge)).toBeFalse();
            await publish(bridge, previewDirectory, page('one'));
            expect(await hasInterface(bridge)).toBeTrue();
        });

        it('keeps the last successful publication after an interrupted or failed one', async () => {
            const {bridge, storage, pending} = memoryBridge();
            await publish(bridge, previewDirectory, page('one'));
            const interrupted = new AbortController();
            interrupted.abort(new Error('Interrupted'));
            await expectAsync(publish(bridge, previewDirectory, page('two'), {signal: interrupted.signal})).toBeRejected();
            expect(pending()).toBe(0);
            expect(await hasInterface(bridge)).toBeTrue();
            expect(new TextDecoder().decode(storage.get(`${previewDirectory}/index.html`))).toBe('one');
        });

        it('is not a preview directory without its page, nor a project that cannot be listed', async () => {
            const {bridge} = memoryBridge(new Map([[`${previewDirectory}/app.js`, encode('')]]));
            expect(await hasInterface(bridge)).toBeFalse();
            expect(await hasInterface({cppFunction: () => async () => { throw new Error('bridge down'); }})).toBeFalse();
        });
    });

    describe('states', () => {
        it('open on the terminal alone without an interface, on the interface alone with one', () => {
            expect(initialState(false)).toBe(State.terminal);
            expect(initialState(true)).toBe(State.interface);
        });

        it('show the icon states only once an interface is displayed', () => {
            for (const event of ['toggle', 'open', 'close', 'hidden']) expect(nextState(State.terminal, event)).toBe(State.terminal);
            // The first preview appears while the terminal is in use: it stays open, over the interface.
            expect(nextState(State.terminal, 'shown')).toBe(State.floating);
        });

        it('open and collapse the terminal over the interface', () => {
            expect(nextState(State.interface, 'toggle')).toBe(State.floating);
            expect(nextState(State.floating, 'toggle')).toBe(State.interface);
            expect(nextState(State.interface, 'open')).toBe(State.floating);
            expect(nextState(State.floating, 'open')).toBe(State.floating);
            expect(nextState(State.floating, 'close')).toBe(State.interface);
            expect(nextState(State.interface, 'close')).toBe(State.interface);
        });

        it('keep their state when wf dev reloads the preview, and return to the terminal without a preview', () => {
            expect(nextState(State.interface, 'shown')).toBe(State.interface);
            expect(nextState(State.floating, 'shown')).toBe(State.floating);
            expect(nextState(State.interface, 'hidden')).toBe(State.terminal);
            expect(nextState(State.floating, 'hidden')).toBe(State.terminal);
            expect(nextState(State.floating, 'unknown')).toBe(State.floating);
        });
    });

    describe('window place', () => {
        const viewport = {width: 1000, height: 700};
        const inside = (rect, view) => rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= view.width && rect.y + rect.height <= view.height;

        it('stays inside the viewport', () => {
            expect(clampRect({x: 100, y: 50, width: 400, height: 300}, viewport)).toEqual({x: 100, y: 50, width: 400, height: 300});
            expect(clampRect({x: -40, y: -10, width: 400, height: 300}, viewport)).toEqual({x: 0, y: 0, width: 400, height: 300});
            expect(clampRect({x: 900, y: 650, width: 400, height: 300}, viewport)).toEqual({x: 600, y: 400, width: 400, height: 300});
            expect(clampRect({x: 10.4, y: 10.6, width: 400.2, height: 300}, viewport)).toEqual({x: 10, y: 11, width: 400, height: 300});
        });

        it('has a minimum size, except in a window smaller than it', () => {
            expect(clampRect({x: 0, y: 0, width: 10, height: 10}, viewport)).toEqual({x: 0, y: 0, ...minimumSize});
            expect(clampRect({x: 0, y: 0, width: 5000, height: 5000}, viewport)).toEqual({x: 0, y: 0, ...viewport});
            const small = {width: 200, height: 120};
            expect(clampRect({x: 50, y: 50, width: 400, height: 300}, small)).toEqual({x: 0, y: 0, ...small});
        });

        it('opens inside viewports of every size', () => {
            for (const view of [viewport, {width: 1920, height: 1080}, {width: 360, height: 240}, {width: 120, height: 80}])
                expect(inside(defaultRect(view), view)).withContext(JSON.stringify(view)).toBeTrue();
        });

        it('moves with the arrow keys and resizes with Shift, inside the viewport', () => {
            const rect = {x: 100, y: 100, width: 400, height: 300};
            expect(keyboardRect(rect, {key: 'ArrowLeft'}, viewport)).toEqual({...rect, x: 100 - step});
            expect(keyboardRect(rect, {key: 'ArrowDown'}, viewport)).toEqual({...rect, y: 100 + step});
            expect(keyboardRect(rect, {key: 'ArrowRight', shiftKey: true}, viewport)).toEqual({...rect, width: 400 + step});
            expect(keyboardRect(rect, {key: 'ArrowUp', shiftKey: true}, viewport)).toEqual({...rect, height: 300 - step});
            expect(keyboardRect({...rect, x: 4}, {key: 'ArrowLeft'}, viewport).x).toBe(0);
            expect(keyboardRect({...rect, x: 600}, {key: 'ArrowRight'}, viewport).x).toBe(600);
            expect(keyboardRect(rect, {key: 'a'}, viewport)).toBeNull();
        });

        it('shows on its tab where wf dev is, else whether a command runs', () => {
            expect(tabStatus()).toBe(Status.idle);
            expect(tabStatus({command: true})).toBe(Status.running);
            for (const dev of [Status.building, Status.ready, Status.failed]) expect(tabStatus({command: true, dev})).toBe(dev);
            // wf dev stopped, or a state this page does not know: the command decides.
            expect(tabStatus({command: true, dev: 'stopped'})).toBe(Status.running);
            expect(tabStatus({command: false, dev: 'toString'})).toBe(Status.idle);
            expect(statusText(Status.idle)).toBe('');
            for (const status of [Status.running, Status.building, Status.ready, Status.failed]) expect(statusText(status)).not.toBe('');
            expect(new Set(Object.values(Status).map(statusText)).size).toBe(5);
        });

        it('is toggled by F9 alone', () => {
            expect(isToggleKey({key: 'F9'})).toBeTrue();
            expect(isToggleKey({key: 'F9', ctrlKey: true})).toBeFalse();
            expect(isToggleKey({key: 'F9', shiftKey: true})).toBeFalse();
            expect(isToggleKey({key: 'c', ctrlKey: true})).toBeFalse();
        });

        it('is toggled by the shortcut of the project\'s manifest instead, which leaves F9 to the application', async () => {
            const manifest = developer => encode(JSON.stringify({format: 1, name: 'app', version: '0.1.0', ...developer}));
            const {bridge} = memoryBridge(new Map([['webfront.project.json', manifest({developer: {terminalShortcut: 'Ctrl+Shift+F9'}})]]));
            const shortcut = await readShortcut(bridge);
            expect(shortcutText(shortcut)).toBe('Ctrl+Shift+F9');
            expect(isToggleKey({key: 'F9'}, shortcut)).toBeFalse();
            expect(isToggleKey({key: 'F9', ctrlKey: true}, shortcut)).toBeFalse();
            expect(isToggleKey({key: 'F9', ctrlKey: true, shiftKey: true, altKey: true}, shortcut)).toBeFalse();
            expect(isToggleKey({key: 'F9', ctrlKey: true, shiftKey: true}, shortcut)).toBeTrue();
            // The manifest alone decides: a lock that is not valid, or not JSON, does not give F9 back to the page.
            for (const lock of ['{"format":99}', '{'])
                expect(shortcutText(await readShortcut(memoryBridge(new Map([['webfront.project.json', manifest({developer: {terminalShortcut: 'Ctrl+Shift+F9'}})],
                    ['webfront-packages.lock.json', encode(lock)]])).bridge))).toBe('Ctrl+Shift+F9');
            // No manifest, no key, or a manifest that is not valid: F9.
            for (const files of [[], [['webfront.project.json', manifest({})]], [['webfront.project.json', manifest({developer: {terminalShortcut: 'Ctrl+C'}})]]])
                expect(shortcutText(await readShortcut(memoryBridge(new Map(files)).bridge))).toBe('F9');
            expect(shortcutText(defaultShortcut)).toBe('F9');
        });

        it('is read from a well-formed file only', () => {
            const rect = {x: 12, y: 34, width: 500, height: 280};
            expect(parsePlace(formatPlace(rect))).toEqual(rect);
            for (const text of ['', '{broken', 'null', '[]', '{"format":2,"x":1,"y":1,"width":400,"height":300}',
                '{"format":1,"x":"1","y":1,"width":400,"height":300}', '{"format":1,"x":1,"y":1,"width":0,"height":300}',
                '{"format":1,"x":1e9,"y":1,"width":400,"height":300}', '{"format":1,"x":1,"y":1,"width":400}'])
                expect(parsePlace(text)).withContext(text).toBeNull();
        });

        it('is remembered in the project, the last one given', async () => {
            const {bridge, storage} = memoryBridge();
            expect(await readPlace(bridge)).toBeNull();
            const store = placeStore(bridge, 60000);
            store.remember({x: 1, y: 2, width: 400, height: 300});
            store.remember({x: 30, y: 40, width: 420, height: 310});
            expect(storage.has(placePath)).toBeFalse();  // a move writes once, when it ends
            await store.saved();
            expect(await readPlace(bridge)).toEqual({x: 30, y: 40, width: 420, height: 310});
            // Another project has its own place.
            expect(await readPlace(memoryBridge().bridge)).toBeNull();
            storage.set(placePath, encode('{broken'));
            expect(await readPlace(bridge)).toBeNull();
        });

        it('keeps the last place chosen when it equals the one written before the write in progress', async () => {
            const {bridge} = memoryBridge();
            const store = placeStore(bridge, 60000);
            const first = {x: 1, y: 2, width: 400, height: 300}, second = {x: 50, y: 60, width: 400, height: 300};
            store.remember(first);
            await store.saved();
            store.remember(second);
            const writing = store.saved();  // not awaited: the first place is chosen again while this one is written
            store.remember(first);
            await Promise.all([writing, store.saved()]);
            expect(await readPlace(bridge)).toEqual(first);
        });

        it('stays usable when the place cannot be written', async () => {
            const store = placeStore({cppFunction: () => async () => { throw new Error('read-only project'); }}, 60000);
            store.remember({x: 1, y: 2, width: 400, height: 300});
            await expectAsync(store.saved()).toBeResolved();
        });
    });

    describe('tab menu', () => {
        describe('access to the inspectors', () => {
            it('is open to an admitted page, whatever the container', () => {
                expect(inspectorAccess([true, true, false])).toEqual({containers: {secondaryWindow: true, host: true, newConnections: false}, reason: ''});
                expect(inspectorAccess([false, true, true]).containers).toEqual({secondaryWindow: false, host: true, newConnections: true});
            });

            it('names no container to a page that is not admitted, and says how to admit one', () => {
                for (const answer of [[false, false, false], [false, false, true]]) {
                    const access = inspectorAccess(answer);
                    expect(access.containers).toBeNull();
                    expect(access.reason).toBe(accessReasons.noContainer);
                }
                expect(accessReasons.noContainer).toContain('trustLoopbackClients');
                expect(accessReasons.noContainer).toContain('embedded frontend');
            });

            it('tells a library without inspectors from a bridge that fails', () => {
                expect(inspectorAccess(null, Object.assign(new Error('not found'), {code: 'webfront.notFound'})).reason).toBe(accessReasons.tooOld);
                expect(inspectorAccess(null, new Error('connection lost')).reason).toContain('connection lost');
                expect(inspectorAccess(['yes', true, true]).containers).toBeNull();
                expect(inspectorAccess([true, true]).containers).toBeNull();
            });

            it('asks the reserved function once', async () => {
                const asked = [];
                const bridge = answer => ({cppFunction: name => async () => { asked.push(name); return answer(); }});
                expect((await readInspectorAccess(bridge(() => [false, true, false]))).containers?.host).toBeTrue();
                expect((await readInspectorAccess(bridge(() => { throw Object.assign(new Error('no'), {code: 'webfront.notFound'}); }))).reason)
                    .toBe(accessReasons.tooOld);
                expect(asked).toEqual([containersFunction, containersFunction]);
            });
        });

        describe('place', () => {
            const viewport = {width: 1000, height: 600}, size = {width: 240, height: 200};
            const tab = {left: 956, right: 1000, top: 64, bottom: 152};

            it('opens on the left of the tab, level with its top', () => {
                expect(placeMenu(tab, size, viewport)).toEqual({x: 716, y: 64, side: 'left'});
            });

            it('opens on the right of an anchor with no room on its left, and ends at its bottom near the window\'s', () => {
                expect(placeMenu({left: 100, right: 300, top: 500, bottom: 540}, size, viewport)).toEqual({x: 300, y: 340, side: 'right'});
            });

            it('stays inside a window smaller than the menu', () => {
                const placed = placeMenu({left: 200, right: 200, top: 90, bottom: 90}, size, {width: 280, height: 140});
                expect(placed.x).toBe(0);
                expect(placed.y).toBe(0);
                expect(placeMenu(tab, {width: 2000, height: 50}, viewport)).toEqual({x: 0, y: 64, side: 'left'});
            });
        });

        describe('on the page', () => {
            let style, button, activated, menu, reason;
            const key = (target, init) => target.dispatchEvent(new KeyboardEvent('keydown', {bubbles: true, cancelable: true, ...init}));
            const items = (level = 0) => [...menu.panels[level].querySelectorAll('[role=menuitem]')];
            const labels = level => items(level).map(item => item.firstChild.textContent);

            beforeEach(() => {
                style = document.createElement('style');
                style.textContent = menuStyles;
                document.head.append(style);
                button = document.createElement('button');
                button.textContent = 'tab';
                button.style.cssText = 'position:fixed;right:0;top:64px;width:44px;height:88px';
                document.body.append(button);
                activated = [];
                reason = 'Not here.';
                menu = createTabMenu({button, activate: id => activated.push(id), entries: [
                    {id: 'terminal', label: 'Terminal', hint: 'F9', keys: 'F9'},
                    ...inspectorMenus.map(group => ({...group, children: group.children.map(panel => ({...panel, reason: () => reason}))})),
                ]});
                button.addEventListener('click', () => { if (!menu.consumeClick()) activated.push('click'); });
            });
            afterEach(() => {
                menu.close({focus: false});
                button.remove();
                style.remove();
            });

            it('opens with a right click, on a menu that names its entries, and leaves the click to the terminal', () => {
                expect(button.getAttribute('aria-haspopup')).toBe('menu');
                expect(menu.isOpen).toBeFalse();
                const event = new MouseEvent('contextmenu', {bubbles: true, cancelable: true, button: 2, clientX: 980, clientY: 100});
                expect(button.dispatchEvent(event)).toBeFalse();  // the browser's own menu does not open
                expect(menu.isOpen).toBeTrue();
                expect(menu.panels[0].querySelector('[role=menu]')).not.toBeNull();
                expect(labels(0)).toEqual(['Terminal', 'C++ state', 'TypeScript state']);
                expect(items(0)[1].getAttribute('aria-haspopup')).toBe('menu');
                expect(items(0)[0].getAttribute('aria-keyshortcuts')).toBe('F9');
                expect(document.activeElement).toBe(items(0)[0]);
                expect(items(0).every(item => item.getBoundingClientRect().height >= 44)).toBeTrue();
                const box = menu.panels[0].getBoundingClientRect();
                expect(box.right).toBeLessThanOrEqual(document.documentElement.clientWidth + 0.5);
                expect(box.left).toBeGreaterThanOrEqual(0);
                button.click();  // a click still toggles the terminal
                expect(activated).toEqual(['click']);
            });

            it('opens with the Menu key and Shift+F10, and is driven with the arrows', () => {
                button.focus();
                key(button, {key: 'F10'});
                expect(menu.isOpen).toBeFalse();
                key(button, {key: 'F10', shiftKey: true});
                expect(menu.isOpen).toBeTrue();
                menu.close();
                expect(document.activeElement).toBe(button);
                key(button, {key: 'ContextMenu'});
                expect(menu.isOpen).toBeTrue();
                const [terminal, cpp, typescript] = items(0);
                key(terminal, {key: 'ArrowDown'});
                expect(document.activeElement).toBe(cpp);
                key(cpp, {key: 'ArrowUp'});
                key(terminal, {key: 'ArrowUp'});
                expect(document.activeElement).toBe(typescript);  // it wraps
                key(typescript, {key: 'Home'});
                expect(document.activeElement).toBe(terminal);
                key(terminal, {key: 'End'});
                key(typescript, {key: 'ArrowUp'});
                key(cpp, {key: 'ArrowRight'});
                expect(cpp.getAttribute('aria-expanded')).toBe('true');
                expect(labels(1)).toEqual(['Functions', 'Browsers and tasks', 'Watches']);
                expect(menu.panels[1].querySelector('[role=menu]').getAttribute('aria-labelledby')).toBe(cpp.id);
                expect(document.activeElement).toBe(items(1)[0]);
                key(items(1)[0], {key: 'ArrowLeft'});
                expect(menu.panels.length).toBe(1);
                expect(document.activeElement).toBe(cpp);
                key(cpp, {key: 'Escape'});
                expect(menu.isOpen).toBeFalse();
                expect(document.activeElement).toBe(button);
                // From a submenu, at any entry, Escape closes every level at once and gives the focus back to the tab.
                for (const group of [1, 2]) {
                    key(button, {key: 'ContextMenu'});
                    key(items(0)[group], {key: 'Enter'});
                    expect(menu.panels.length).toBe(2);
                    const last = items(1).at(-1);
                    key(last, {key: 'End'});
                    key(last, {key: 'Escape'});
                    expect(menu.isOpen).withContext(`group ${group}`).toBeFalse();
                    expect(document.querySelector('.webfront-menu')).toBeNull();
                    expect(document.activeElement).toBe(button);
                }
            });

            it('greys an unavailable entry with its reason, and does nothing when it is chosen', () => {
                button.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}));
                key(items(0)[2], {key: 'ArrowRight'});
                expect(labels(1)).toEqual(['Packages', 'Bridge']);
                const note = menu.panels[1].querySelector('.webfront-menu-reason');
                expect(note.textContent).toBe('Not here.');
                for (const item of items(1)) {
                    expect(item.getAttribute('aria-disabled')).toBe('true');
                    expect(item.getAttribute('aria-describedby')).toBe(note.id);
                    expect(item.title).toBe('Not here.');
                }
                key(items(1)[0], {key: 'Enter'});
                items(1)[1].click();
                expect(activated).toEqual([]);
                expect(menu.isOpen).toBeTrue();
                // The reasons are read each time a submenu opens.
                reason = '';
                key(items(1)[0], {key: 'ArrowLeft'});
                key(items(0)[2], {key: 'ArrowRight'});
                expect(items(1).some(item => item.hasAttribute('aria-disabled'))).toBeFalse();
                expect(getComputedStyle(menu.panels[1].querySelector('.webfront-menu-reason')).display).toBe('none');
                key(items(1)[1], {key: 'Enter'});
                expect(activated).toEqual(['typescript.bridge']);
                expect(menu.isOpen).toBeFalse();
            });

            it('activates the terminal, and closes on a press elsewhere without taking the focus', () => {
                button.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}));
                key(items(0)[0], {key: ' '});
                expect(activated).toEqual(['terminal']);
                expect(menu.isOpen).toBeFalse();
                const field = document.createElement('input');
                document.body.append(field);
                try {
                    button.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}));
                    document.body.dispatchEvent(new PointerEvent('pointerdown', {bubbles: true}));
                    expect(menu.isOpen).toBeFalse();
                    button.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}));
                    field.focus();
                    // A window without the system's focus moves the focus without sending focusin: the spec sends it.
                    field.dispatchEvent(new FocusEvent('focusin', {bubbles: true}));
                    expect(menu.isOpen).toBeFalse();
                    expect(document.activeElement).toBe(field);
                } finally { field.remove(); }
            });

            it('reads the reasons again on refresh, in the panels already open', () => {
                button.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}));
                key(items(0)[1], {key: 'ArrowRight'});
                const [panel, first] = [menu.panels[1], items(1)[0]];
                expect(first.getAttribute('aria-disabled')).toBe('true');
                reason = '';
                menu.refresh();
                expect(menu.panels[1]).toBe(panel);
                expect(document.activeElement).toBe(first);
                expect(items(1).some(item => item.hasAttribute('aria-disabled') || item.hasAttribute('aria-describedby') || item.title)).toBeFalse();
                expect(panel.querySelector('.webfront-menu-reason').textContent).toBe('');
                reason = 'Gone.';
                menu.refresh();
                expect(items(1).every(item => item.getAttribute('aria-disabled') === 'true' && item.title === 'Gone.')).toBeTrue();
                expect(panel.querySelector('.webfront-menu-reason').textContent).toBe('Gone.');
                expect(items(0).some(item => item.hasAttribute('aria-disabled'))).toBeFalse();  // the groups stay available
            });

            it('places the open panels again when a longer reason makes them taller, near the bottom of the window', () => {
                const height = document.documentElement.clientHeight, width = document.documentElement.clientWidth;
                const inside = panel => {
                    const box = panel.getBoundingClientRect();
                    return box.top >= 0 && box.bottom <= height + 0.5 && box.left >= 0 && box.right <= width + 0.5;
                };
                reason = 'Wait.';
                menu.open({left: width - 44, right: width, top: height - 88, bottom: height});
                key(items(0)[2], {key: 'ArrowRight'});
                const [root, submenu] = menu.panels, before = submenu.getBoundingClientRect().height;
                expect(inside(root) && inside(submenu)).toBeTrue();
                reason = 'A reason long enough to take several lines in the submenu. '.repeat(4);
                menu.refresh();
                expect(submenu.getBoundingClientRect().height).toBeGreaterThan(before + 20);
                expect(menu.panels).toEqual([root, submenu]);
                expect(inside(root) && inside(submenu)).withContext(JSON.stringify(submenu.getBoundingClientRect())).toBeTrue();
                expect(document.activeElement).toBe(items(1)[0]);
            });

            it('leaves the menu with Tab and Shift+Tab, from the tab, in the direction asked', () => {
                for (const shiftKey of [false, true]) {
                    for (const depth of [0, 1]) {
                        button.focus();
                        key(button, {key: 'ContextMenu'});
                        if (depth) key(items(0)[1], {key: 'ArrowRight'});
                        const from = document.activeElement;
                        const tab = new KeyboardEvent('keydown', {bubbles: true, cancelable: true, key: 'Tab', shiftKey});
                        from.dispatchEvent(tab);
                        // The browser moves the focus after the event, from the element that has it then: the tab.
                        expect(tab.defaultPrevented).withContext(`shift ${shiftKey}, level ${depth}`).toBeFalse();
                        expect(menu.isOpen).toBeFalse();
                        expect(document.querySelector('.webfront-menu')).toBeNull();
                        expect(document.activeElement).toBe(button);
                    }
                }
            });

            it('takes no key while it is closed', () => {
                const seen = [];
                const listener = event => seen.push(event.key);
                window.addEventListener('keydown', listener);
                try {
                    for (const name of ['Escape', 'ArrowDown', 'Enter']) key(document.body, {key: name});
                    expect(seen).toEqual(['Escape', 'ArrowDown', 'Enter']);
                } finally { window.removeEventListener('keydown', listener); }
            });

            it('opens with a long press on a touch screen, without toggling the terminal', () => {
                jasmine.clock().install();
                try {
                    const touch = (type, x = 980, y = 100) => button.dispatchEvent(new PointerEvent(type, {bubbles: true, pointerType: 'touch', clientX: x, clientY: y}));
                    touch('pointerdown');
                    jasmine.clock().tick(longPress.delay - 1);
                    expect(menu.isOpen).toBeFalse();
                    jasmine.clock().tick(1);
                    expect(menu.isOpen).toBeTrue();
                    touch('pointerup');
                    button.click();
                    expect(activated).toEqual([]);
                    menu.close();
                    // A short tap is a click; a press that moves is not a long press.
                    touch('pointerdown');
                    touch('pointerup');
                    button.click();
                    expect(activated).toEqual(['click']);
                    touch('pointerdown');
                    touch('pointermove', 980, 100 + longPress.distance + 5);
                    jasmine.clock().tick(longPress.delay * 2);
                    expect(menu.isOpen).toBeFalse();
                } finally { jasmine.clock().uninstall(); }
            });

            it('ends the long press when its pointer is cancelled after the menu opened', () => {
                jasmine.clock().install();
                try {
                    const touch = type => button.dispatchEvent(new PointerEvent(type, {bubbles: true, pointerType: 'touch', clientX: 980, clientY: 100}));
                    touch('pointerdown');
                    jasmine.clock().tick(longPress.delay);
                    expect(menu.isOpen).toBeTrue();
                    touch('pointercancel');  // no pointerup, no click: nothing else comes from this touch
                    // The tab is a tab again at once, with no delay to wait for.
                    expect(button.dispatchEvent(new MouseEvent('mousedown', {bubbles: true, cancelable: true}))).toBeTrue();
                    button.focus();
                    button.dispatchEvent(new FocusEvent('focusin', {bubbles: true}));
                    expect(menu.isOpen).toBeFalse();
                    expect(document.activeElement).toBe(button);
                    button.click();  // Enter or Space on the tab
                    expect(activated).toEqual(['click']);
                } finally { jasmine.clock().uninstall(); }
            });

            it('stays open after a long press when the engine sends mouse events once the finger lifts', () => {
                jasmine.clock().install();
                try {
                    const touch = type => button.dispatchEvent(new PointerEvent(type, {bubbles: true, pointerType: 'touch', clientX: 980, clientY: 100}));
                    const mouseDown = () => button.dispatchEvent(new MouseEvent('mousedown', {bubbles: true, cancelable: true}));
                    touch('pointerdown');
                    jasmine.clock().tick(longPress.delay);
                    touch('pointerup');
                    const first = items(0)[0];
                    expect(document.activeElement).toBe(first);
                    // The compatibility mousedown does not move the focus, and an engine that moves it anyway gives it back.
                    expect(mouseDown()).toBeFalse();
                    button.focus();
                    button.dispatchEvent(new FocusEvent('focusin', {bubbles: true}));
                    expect(menu.isOpen).toBeTrue();
                    expect(document.activeElement).toBe(first);
                    button.click();
                    expect(menu.isOpen).toBeTrue();
                    expect(activated).toEqual([]);
                    // Once that touch is over, the tab is a tab again: a press closes the menu, a mousedown is the browser's.
                    expect(mouseDown()).toBeTrue();
                    menu.close({focus: false});
                    // An engine that sends no click: the touch is over shortly after the finger lifts.
                    touch('pointerdown');
                    jasmine.clock().tick(longPress.delay);
                    touch('pointerup');
                    jasmine.clock().tick(longPress.settle);
                    expect(mouseDown()).toBeTrue();
                    button.focus();
                    button.dispatchEvent(new FocusEvent('focusin', {bubbles: true}));
                    expect(menu.isOpen).toBeFalse();
                    button.click();
                    expect(activated).toEqual(['click']);
                } finally { jasmine.clock().uninstall(); }
            });
        });

        // A workspace of this page on a project in memory, whose webfront.inspector.containers() answers `containers`.
        const press = (target, init) => target.dispatchEvent(new KeyboardEvent('keydown', {bubbles: true, cancelable: true, ...init}));
        async function testWorkspace({files = new Map(), containers = Promise.resolve([false, true, false])} = {}) {
            const {bridge: projectBridge} = memoryBridge(files);
            const bridge = {cppFunction: name => name === containersFunction ? () => containers : projectBridge.cppFunction(name)};
            const element = document.createElement('div'), frame = document.createElement('iframe');
            frame.hidden = true;
            document.body.append(element, frame);
            const workspace = await createWorkspace({bridge, element, frame, openInspector: () => {}});
            const remove = async () => {
                workspace.menu.close({focus: false});
                frame.hidden = true;  // the layout goes back to the terminal alone: its listeners on this window do nothing
                await new Promise(resolve => setTimeout(resolve));
                [workspace.panel, workspace.button, frame].forEach(node => node.remove());
                document.head.querySelectorAll('style').forEach(style => { if (style.textContent.includes('.webfront-terminal-toggle')) style.remove(); });
                document.documentElement.removeAttribute('data-webfront-workspace');
            };
            return {workspace, frame, remove};
        }

        describe('with Shift+F10 as the terminal\'s shortcut', () => {
            let mounted;
            beforeEach(async () => {
                const manifest = encode(JSON.stringify({format: 1, name: 'app', version: '0.1.0', developer: {terminalShortcut: 'Shift+F10'}}));
                const files = new Map([['webfront.project.json', manifest], [`${previewDirectory}/index.html`, encode('<p>app')]]);
                mounted = await testWorkspace({files});
            });
            afterEach(() => mounted.remove());

            it('opens the menu of the focused tab, and toggles the terminal anywhere else', () => {
                const {workspace} = mounted;
                expect(workspace.shortcut).toBe('Shift+F10');
                expect(workspace.state).toBe(State.interface);
                workspace.button.focus();
                press(workspace.button, {key: 'F10', shiftKey: true});
                expect(workspace.menu.isOpen).toBeTrue();
                expect(workspace.state).toBe(State.interface);
                // In the menu, the shortcut is the terminal's again: it closes the menu and opens the terminal.
                press(document.activeElement, {key: 'F10', shiftKey: true});
                expect(workspace.menu.isOpen).toBeFalse();
                expect(workspace.state).toBe(State.floating);
                press(document.body, {key: 'F10', shiftKey: true});
                expect(workspace.state).toBe(State.interface);
                expect(workspace.menu.isOpen).toBeFalse();
                // From the tab, the terminal is still a click (Enter or Space) away.
                workspace.button.click();
                expect(workspace.state).toBe(State.floating);
            });
        });

        it('fits a window narrower than its usual width', async () => {
            const host = document.createElement('iframe');
            host.style.cssText = 'position:fixed;left:0;top:0;width:180px;height:400px;border:0';
            const loaded = new Promise(resolve => host.addEventListener('load', resolve, {once: true}));
            host.srcdoc = '<!doctype html><body style="margin:0">';
            document.body.append(host);
            await loaded;
            try {
                const page = host.contentDocument, style = page.createElement('style'), tab = page.createElement('button');
                style.textContent = menuStyles;
                tab.style.cssText = 'position:fixed;right:0;top:64px;width:44px;height:88px';
                page.head.append(style);
                page.body.append(tab);
                const narrow = createTabMenu({button: tab, activate: () => {}, entries: [
                    {id: 'terminal', label: 'Terminal', hint: 'F9'},
                    ...inspectorMenus.map(group => ({...group, children: group.children.map(panel => ({...panel, reason: () => accessReasons.noContainer}))})),
                ]});
                narrow.open();
                press(page.activeElement, {key: 'ArrowDown'});
                press(page.activeElement, {key: 'ArrowRight'});
                expect(narrow.panels.length).toBe(2);
                for (const panel of narrow.panels) {
                    const box = panel.getBoundingClientRect();
                    expect(box.left >= 0 && box.right <= 180.5 && box.width <= 180).withContext(JSON.stringify(box)).toBeTrue();
                }
                narrow.close({focus: false});
            } finally { host.remove(); }
        });

        describe('with an answer of C++ that comes after the submenu opened', () => {
            let mounted, workspace, answer;

            beforeEach(async () => {
                mounted = await testWorkspace({containers: new Promise(resolve => { answer = resolve; })});
                workspace = mounted.workspace;
            });
            afterEach(() => mounted.remove());
            // Opens the submenu of `group` (1: C++ state, 2: TypeScript state) while C++ has not answered yet.
            const openSubmenu = (group, anchor) => {
                workspace.menu.open(anchor);
                press(workspace.menu.panels[0].querySelectorAll('[role=menuitem]')[group], {key: 'ArrowRight'});
                const submenu = workspace.menu.panels[1];
                expect(submenu.querySelector('.webfront-menu-reason').textContent).toBe(accessReasons.checking);
                return submenu;
            };

            it('enables the entries of an admitted page', async () => {
                const submenu = openSubmenu(1), entries = [...submenu.querySelectorAll('[role=menuitem]')];
                expect(entries.every(entry => entry.getAttribute('aria-disabled') === 'true')).toBeTrue();
                answer([false, true, false]);
                await workspace.inspectorAccess;
                expect(workspace.menu.panels[1]).toBe(submenu);
                expect(submenu.querySelector('.webfront-menu-reason').textContent).toBe('');
                expect(entries.some(entry => entry.hasAttribute('aria-disabled'))).toBeFalse();
            });

            it('keeps a submenu opened near the bottom of the window inside it once its reason is longer', async () => {
                const height = document.documentElement.clientHeight, width = document.documentElement.clientWidth;
                const submenu = openSubmenu(2, {left: width - 44, right: width, top: height - 88, bottom: height});
                const before = submenu.getBoundingClientRect();
                expect(before.bottom).toBeLessThanOrEqual(height + 0.5);
                answer([false, false, false]);  // a page that is not admitted: the longest reason
                await workspace.inspectorAccess;
                const after = submenu.getBoundingClientRect();
                expect(submenu.querySelector('.webfront-menu-reason').textContent).toBe(accessReasons.noContainer);
                expect(after.height).toBeGreaterThan(before.height);
                expect(after.top >= 0 && after.bottom <= height + 0.5).withContext(JSON.stringify(after)).toBeTrue();
                for (const panel of workspace.menu.panels) {
                    const box = panel.getBoundingClientRect();
                    expect(box.left >= 0 && box.right <= width + 0.5 && box.top >= 0 && box.bottom <= height + 0.5).toBeTrue();
                }
            });
        });
    });
});
