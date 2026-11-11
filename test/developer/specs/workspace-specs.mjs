// Specs of the developer page's layout (workspace.mjs): what counts as an existing interface, the states of the display,
// and the place of the floating terminal window. The page itself is checked in test/web/developer-specs.mjs.
import {hasInterface, previewDirectory, State, initialState, nextState, clampRect, defaultRect, keyboardRect, minimumSize, step,
    isToggleKey, Status, tabStatus, statusText, defaultShortcut, shortcutText, readShortcut, parsePlace, formatPlace, readPlace, placeStore, placePath} from '../../../js/developer/environment/workspace.mjs';
import {publish} from '../../../js/developer/environment/io.mjs';
import {memoryBridge} from '../memory-bridge.mjs';

describe('The developer page layout', () => {
    const encode = text => new TextEncoder().encode(text);
    const page = text => new Map([['index.html', encode(text)], ['app.js', encode('')]]);

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
});
