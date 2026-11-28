// Specs of the trees of the watches (#682, ADR-027 sections 3 to 6): the nodes and the pages, the order of the calls that
// change C++'s memory, the passes of differences and what interrupts them, the resynchronization, writes and full values,
// and the view that shows a tree. A fake of the C++ side answers here; the real one is read in a browser by
// test/web/suite/specs/inspector-specs.mjs.
import {flags, functions, codes, readNode, readNodePage, readLimits, writeBound, utf8Bytes, parentOf, createWatchTree, createTreeView,
    writeFailure, blockWidth, layouts, keptLayout, keepLayout, keptLayoutCount, watchTreeStyles} from '../../../js/developer/environment/inspector-watches.mjs';

const node = (path, kind, label, value, children = 0, bits = 0) => [path, kind, label, value, children, bits];
const refusal = (code, message = code) => Object.assign(new Error(message), {code});
const limits = [4096, 32768, 10000, 16 * 1024 * 1024, 1000, 200];

/**
 * A fake of C++ for one watch: a vector of texts, with a memory under a revision, as WatchMemory.hpp keeps one. Its calls
 * are recorded; `hold(name)` makes the next call of that function wait until the function it returns is called.
 */
function fakeWatch(values, {writable = true, pageSize = Infinity} = {}) {
    const calls = [];
    const held = new Map();
    let revision = 1, loaded = new Set(), pass = null, snapshot = 0;
    const memory = new Map();
    const bits = writable ? flags.writable : 0;
    const root = () => node('', 'sequence', 'values', '', values.length);
    const child = index => node(`/#${index}`, 'string', `[${index}]`, values[index], 0, bits);
    const resync = () => { loaded = new Set(); memory.clear(); pass = null; return [[], 0, ++revision, true]; };
    const answers = {
        [functions.limits]: () => limits,
        [functions.reset]: () => { loaded = new Set(); memory.clear(); pass = null; return ++revision; },
        [functions.children]: (watch, path, asked, start, count) => {
            if (asked !== revision) return resync();
            pass = null;
            if (path !== '') {
                const index = Number(path.slice(2));
                if (!loaded.has(index)) throw refusal(codes.unknown);
                if (index >= values.length) return [[node(path, 'string', `[${index}]`, '', 0, flags.removed)], 0, ++revision, false];
                memory.set(index, values[index]);
                return [[child(index)], 0, ++revision, false];
            }
            const served = [];
            for (let index = start; index < values.length && served.length < Math.min(count, pageSize); ++index) {
                loaded.add(index);
                memory.set(index, values[index]);
                served.push(child(index));
            }
            const after = start + served.length;
            return [[root(), ...served], after < values.length ? after : 0, ++revision, false];
        },
        [functions.changes]: (watch, asked, cursor, count) => {
            if (asked !== revision) return resync();
            if (cursor === 0) {
                pass = [...loaded].filter(index => index < values.length && memory.get(index) !== values[index]).map(index => {
                    memory.set(index, values[index]);
                    return child(index);
                });
                ++revision;
            } else if (!pass) return resync();
            const served = pass.splice(0, Math.min(count, pageSize));
            const next = pass.length ? cursor + served.length : 0;
            if (!next) pass = null;
            return [served, next, revision, false];
        },
        [functions.fold]: (watch, path, asked) => {
            if (asked !== revision) return resync();
            pass = null;
            if (path === '') { loaded = new Set(); memory.clear(); }
            return [[], 0, ++revision, false];
        },
        [functions.write]: (watch, asked, path, expected, text) => {
            if (asked !== revision) throw refusal(codes.stale);
            pass = null;
            const index = Number(path.slice(2));
            if (values[index] !== expected) return [++revision, codes.stale, ''];
            if (text === 'refuse') return [++revision, codes.refused, 'not this value'];
            values[index] = text;
            memory.set(index, text);
            return [++revision, '', ''];
        },
        [functions.value]: (watch, path, asked, offset) => {
            const text = values[Number(path.slice(2))].repeat(3);
            if (asked === 0) return [text.slice(0, 4), text.length, text.length > 4 ? 4 : 0, false, ++snapshot];
            if (asked !== snapshot) throw refusal(codes.stale);
            const end = offset + 4;
            return [text.slice(offset, end), text.length, end < text.length ? end : 0, false, snapshot];
        },
    };
    return {
        calls,
        answers,
        get revision() { return revision; },
        get passing() { return pass !== null; },
        /** Makes the next call of `name` wait; the returned function lets it go on. */
        hold(name) {
            let release;
            held.set(name, new Promise(resolve => { release = resolve; }));
            return release;
        },
        cppFunction: name => async (...args) => {
            calls.push([name, ...args]);
            const waiting = held.get(name);
            if (waiting) {
                held.delete(name);
                await waiting;
            }
            return answers[name](...args);
        },
    };
}

const names = bridge => bridge.calls.map(([name]) => name.replace('webfront.inspector.', ''));
const valuesOf = tree => [...tree.nodes.values()].map(({node: shown}) => shown.value);

describe('The nodes and pages of a watch', () => {
    it('are read from the tuples C++ answers', () => {
        expect(readNode(node('/#1', 'integer', '[1]', '42', 0, 3))).toEqual({path: '/#1', kind: 'integer', label: '[1]', value: '42', children: 0, flags: 3});
        expect(readNodePage([[node('', 'sequence', 'grid', '', 2)], 0, 7, false]))
            .toEqual({nodes: [{path: '', kind: 'sequence', label: 'grid', value: '', children: 2, flags: 0}], next: 0, revision: 7, resync: false});
        expect(readLimits(limits).maxWriteTextBytes).toBe(4096);
    });

    it('are refused when they are not what C++ sends', () => {
        for (const bad of [null, [], ['', 'k', 'l', 'v', 0], ['', 'k', 'l', 1, 0, 0], ['', 'k', 'l', 'v', -1, 0], ['', 'k', 'l', 'v', 0.5, 0]])
            expect(() => readNode(bad)).toThrowError(/node/);
        for (const bad of [null, [[], 0, 1], [{}, 0, 1, false], [[], 0, 1, 0], [[], -1, 1, false]]) expect(() => readNodePage(bad)).toThrowError(/page/);
        expect(() => readLimits([1, 2])).toThrowError(/limits/);
    });

    it('have paths whose parent is the path without its last segment', () => {
        expect(parentOf('/#1/.2/kab')).toBe('/#1/.2');
        expect(parentOf('/#1')).toBe('');
    });

    it('bound the text of a write in UTF-8 bytes, by C++ and by the page\'s own codec', () => {
        expect(utf8Bytes('é€')).toBe(5);
        expect(writeBound(readLimits(limits))).toBe(4096);
        expect(writeBound(readLimits(limits), 2000)).toBe(2000 - 1343);
        expect(writeBound(null)).toBe(0);
    });
});

describe('The tree of a watch', () => {
    it('opens on an empty memory: reset, then the root and its children', async () => {
        const bridge = fakeWatch(['a', 'b']);
        let updates = 0;
        const tree = createWatchTree({bridge, watch: 7, onUpdate: () => ++updates});
        await tree.open();
        expect(names(bridge)).toEqual(['limits', 'reset', 'children']);
        expect(bridge.calls[2]).toEqual([functions.children, 7, '', 2, 0, 200]);
        expect([...tree.nodes.keys()]).toEqual(['', '/#0', '/#1']);
        expect(tree.nodes.get('').positions).toEqual(new Map([[0, '/#0'], [1, '/#1']]));
        expect(tree.revision).toBe(bridge.revision);
        expect(tree.expanded.has('')).toBeTrue();
        expect(updates).toBeGreaterThan(0);
    });

    it('sends a call that changes the memory only after the answer of the previous one, with its revision', async () => {
        const bridge = fakeWatch(['a', 'b', 'c']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const release = bridge.hold(functions.fold);
        const before = bridge.calls.length;
        const folded = tree.collapse('');
        const unfolded = tree.expand('');
        await Promise.resolve();
        await Promise.resolve();
        expect(bridge.calls.length).toBe(before + 1);  // the unfolding waits for the answer of the folding
        release();
        await Promise.all([folded, unfolded]);
        const [, , , asked] = bridge.calls.at(-1);
        expect(names(bridge).slice(before)).toEqual(['fold', 'children']);
        expect(asked).toBe(tree.revision - 1);  // the revision the folding answered
        expect(tree.nodes.size).toBe(4);
    });

    it('reads the next page of a list, and any window of a large container', async () => {
        const bridge = fakeWatch(Array.from({length: 10}, (unused, index) => `v${index}`), {pageSize: 4});
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        expect(tree.nodes.get('').next).toBe(4);
        await tree.load('', 8);
        expect([...tree.nodes.get('').positions.keys()]).toEqual([0, 1, 2, 3, 8, 9]);
        expect(tree.nodes.get('/#9').node.value).toBe('v9');
    });

    it('reads a pass of differences, and marks what it changed', async () => {
        const values = ['a', 'b', 'c'];
        const bridge = fakeWatch(values);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        values[1] = 'B';
        expect(await tree.refresh()).toBeTrue();
        expect(tree.nodes.get('/#1').node.value).toBe('B');
        expect(tree.nodes.get('/#1').changed).toBe(tree.pass);
        expect(tree.nodes.get('/#0').changed).toBe(0);
        await tree.refresh();
        expect(tree.nodes.get('/#1').changed).not.toBe(tree.pass);  // nothing changed in the last pass
    });

    it('does not make an unfolding wait for the end of a pass: the pass stops, and the next one finds the rest', async () => {
        const values = Array.from({length: 6}, (unused, index) => `v${index}`);
        const bridge = fakeWatch(values, {pageSize: 2});
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        await tree.load('', 2);
        await tree.load('', 4);
        for (let index = 0; index < 6; ++index) values[index] = `w${index}`;
        const release = bridge.hold(functions.changes);
        const before = bridge.calls.length;
        const passing = tree.refresh();
        await Promise.resolve();
        const reading = tree.load('', 0);  // asked while the first page of the pass is awaited
        release();
        await Promise.all([passing, reading]);
        // One page of the pass, then the unfolding: the second page of the pass was never asked.
        expect(names(bridge).slice(before)).toEqual(['changes', 'children']);
        expect(valuesOf(tree).slice(1)).toEqual(['w0', 'w1', 'v2', 'v3', 'v4', 'v5']);
        expect(bridge.passing).toBeFalse();
        // The period reads only when nothing else does.
        const held = bridge.hold(functions.children);
        const busy = tree.load('', 2);
        expect(await tree.refresh()).toBeFalse();
        held();
        await busy;
    });

    it('reads everything again from the root after a resynchronization, with the paths that were unfolded', async () => {
        const bridge = fakeWatch(['a', 'b']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        bridge.answers[functions.reset]();  // C++'s memory moved without this page: its revision is stale
        const before = bridge.calls.length;
        await tree.refresh();
        expect(names(bridge).slice(before)).toEqual(['changes', 'children']);
        expect([...tree.nodes.keys()]).toEqual(['', '/#0', '/#1']);
        expect(tree.revision).toBe(bridge.revision);
        expect(await tree.refresh()).toBeTrue();
        expect(names(bridge).at(-1)).toBe('changes');
    });

    it('reads a list again when a pass tells that its children changed', async () => {
        const values = ['a', 'b'];
        const bridge = fakeWatch(values);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        values.push('c');
        const changes = bridge.answers[functions.changes];
        bridge.answers[functions.changes] = (...args) => {
            bridge.answers[functions.changes] = changes;
            const [, asked] = args;
            const page = changes(...args);
            return asked === page[2] ? page : [[node('', 'sequence', 'values', '', 3, flags.childrenChanged)], 0, page[2], false];
        };
        await tree.refresh();
        await tree.settled();
        expect(names(bridge).slice(-2)).toEqual(['changes', 'children']);
        expect([...tree.nodes.keys()]).toEqual(['', '/#0', '/#1', '/#2']);
        expect(tree.nodes.get('').changed).toBe(tree.pass);
    });

    it('folds a node: the page forgets what C++ forgets', async () => {
        const bridge = fakeWatch(['a', 'b']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        await tree.collapse('');
        expect([...tree.nodes.keys()]).toEqual(['']);
        expect(tree.expanded.has('')).toBeFalse();
        expect(tree.revision).toBe(bridge.revision);
    });

    it('writes a value with what it shows, and adopts the revision of the answer', async () => {
        const values = ['a', 'b'];
        const bridge = fakeWatch(values);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        expect(await tree.write('/#1', 'B')).toEqual({code: '', message: ''});
        expect(bridge.calls.find(([name]) => name === functions.write).slice(3)).toEqual(['/#1', 'b', 'B']);
        expect(values).toEqual(['a', 'B']);
        expect(tree.nodes.get('/#1').node.value).toBe('B');
        expect(tree.revision).toBe(bridge.revision);
        expect(await tree.write('/#1', 'refuse')).toEqual({code: codes.refused, message: 'not this value'});
        expect(tree.revision).toBe(bridge.revision);
    });

    it('reads a node again after a stale refusal, so that the write is offered on what the object shows', async () => {
        const values = ['a', 'b'];
        const bridge = fakeWatch(values);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        values[1] = 'changed';
        const answer = await tree.write('/#1', 'B');
        expect(answer.code).toBe(codes.stale);
        expect(values[1]).toBe('changed');
        expect(tree.nodes.get('/#1').node.value).toBe('changed');
        expect(names(bridge).slice(-2)).toEqual(['write', 'children']);
        expect((await tree.write('/#1', 'B')).code).toBe('');
    });

    it('checks what the edition was started on just before it sends the write', async () => {
        const bridge = fakeWatch(['a', 'b']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const shown = tree.shown('/#1');
        const release = bridge.hold(functions.fold);
        const folding = tree.collapse('/#0');  // the write waits behind it
        const writing = tree.write('/#1', 'B', 'b', shown);
        tree.nodes.get('/#1').node.label = 'another';  // what a pass answered meanwhile would change
        release();
        await folding;
        expect((await writing).code).toBe(codes.stale);
        expect(bridge.calls.some(([name]) => name === functions.write)).toBeFalse();
    });

    it('refuses in the page, without a call, a text longer than the bound', async () => {
        const bridge = fakeWatch(['a']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const before = bridge.calls.length;
        expect((await tree.write('/#0', 'é'.repeat(2049))).code).toBe(codes.tooLarge);  // 4 098 bytes
        expect(bridge.calls.length).toBe(before);
        expect((await tree.write('/#0', 'é'.repeat(2048))).code).toBe('');
        const narrow = createWatchTree({bridge, watch: 1, maxMessageBytes: 1343 + 10});
        await narrow.open();
        expect((await narrow.write('/#0', 'x'.repeat(11))).code).toBe(codes.tooLarge);
    });

    it('reads a full value by fragments of one snapshot, and starts again when another reading replaced it', async () => {
        const bridge = fakeWatch(['abcdefgh']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        expect(await tree.fullValue('/#0')).toEqual({text: 'abcdefgh'.repeat(3), total: 24, capped: false});
        const value = bridge.answers[functions.value];
        let interleaved = false;
        bridge.answers[functions.value] = (...args) => {
            if (!interleaved && args[2] !== 0) {
                interleaved = true;
                value(1, '/#0', 0, 0);  // another reading takes the snapshot
            }
            return value(...args);
        };
        expect((await tree.fullValue('/#0')).text).toBe('abcdefgh'.repeat(3));
        expect(interleaved).toBeTrue();
        bridge.answers[functions.value] = () => { throw refusal(codes.stale); };
        await expectAsync(tree.fullValue('/#0')).toBeRejectedWithError(codes.stale);
    });

    it('keeps two keyed elements that swapped their places when their page is read again', async () => {
        const keyed = order => [[node('', 'sequence', 'rows', '', 2), ...order.map(key => node(`/k${key}`, 'integer', key, key === 'a' ? '1' : '2'))],
            0, 0, false];
        let order = ['a', 'b'], revision = 1;
        const answers = {
            [functions.limits]: () => limits,
            [functions.reset]: () => ++revision,
            [functions.children]: () => { const page = keyed(order); page[2] = ++revision; return page; },
        };
        const bridge = {cppFunction: name => async (...args) => answers[name](...args)};
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        order = ['b', 'a'];
        await tree.load('', 0);
        expect([...tree.nodes.keys()].sort()).toEqual(['', '/ka', '/kb']);
        expect([...tree.nodes.get('').positions.values()]).toEqual(['/kb', '/ka']);
        // An element that left the page leaves the tree.
        order = ['b', 'c'];
        await tree.load('', 0);
        expect([...tree.nodes.keys()].sort()).toEqual(['', '/kb', '/kc']);
    });

    it('settles what was waiting when it is stopped: no caller is left pending', async () => {
        const bridge = fakeWatch(['a', 'b']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const release = bridge.hold(functions.fold);
        const folding = tree.collapse('');
        const unfolding = tree.expand('');  // waits behind the folding
        tree.stop();
        await expectAsync(unfolding).toBeRejectedWithError('The watch is gone');
        release();
        await folding;
    });

    it('reads nothing more once it is stopped, also when a pass answers a resynchronization', async () => {
        const bridge = fakeWatch(['a', 'b']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        bridge.answers[functions.reset]();  // C++'s memory moved: the next pass resynchronizes
        const release = bridge.hold(functions.changes);
        const passing = tree.refresh();
        await Promise.resolve();
        tree.stop();  // the panel closes while the pass is awaited
        const before = bridge.calls.length;
        release();
        await passing;
        expect(bridge.calls.slice(before).some(([name]) => name === functions.children)).toBeFalse();
    });

    it('keeps a write that C++ confirmed when only the reading again fails', async () => {
        const values = ['a', 'b'];
        const bridge = fakeWatch(values);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const children = bridge.answers[functions.children];
        bridge.answers[functions.write] = (...args) => {
            bridge.answers[functions.children] = () => { throw Object.assign(new Error('the executor has no loop'), {code: codes.unavailable}); };
            return [bridge.revision + 1, '', ''];
        };
        const answer = await tree.write('/#1', 'B');
        expect(answer.code).toBe('');
        expect(answer.refresh).toContain('no loop');
        bridge.answers[functions.children] = children;
    });

    it('keeps unfolded a list whose next page the full memory refuses, so that it can be folded', async () => {
        const bridge = fakeWatch(Array.from({length: 6}, (unused, index) => `v${index}`), {pageSize: 2});
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const children = bridge.answers[functions.children];
        bridge.answers[functions.children] = () => { throw Object.assign(new Error('memory full'), {code: codes.limit}); };
        await tree.load('', 2);
        expect(tree.expanded.has('')).toBeTrue();
        expect(tree.problem).toBe('memory full');
        bridge.answers[functions.children] = children;
    });

    it('builds the tree again at the next refresh when the reading of a resynchronization fails', async () => {
        const bridge = fakeWatch(['a', 'b']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        bridge.answers[functions.reset]();  // the next pass answers a resynchronization
        const children = bridge.answers[functions.children];
        bridge.answers[functions.children] = () => { throw Object.assign(new Error('the executor has no loop'), {code: codes.unavailable}); };
        await expectAsync(tree.refresh()).toBeRejected();
        expect(tree.nodes.size).toBe(0);
        bridge.answers[functions.children] = children;
        expect(await tree.refresh()).toBeTrue();
        expect([...tree.nodes.keys()]).toEqual(['', '/#0', '/#1']);
    });

    it('sends nothing more when it is closed while it opens, or while it reads a full value', async () => {
        const bridge = fakeWatch(['abcdefgh']);
        const opened = createWatchTree({bridge, watch: 1});
        await opened.open();
        const tree = createWatchTree({bridge, watch: 1});
        const release = bridge.hold(functions.limits);
        const opening = tree.open();
        await Promise.resolve();
        tree.stop();
        const before = bridge.calls.length;
        release();
        await opening.catch(() => {});
        expect(bridge.calls.slice(before).map(([name]) => name)).not.toContain(functions.reset);
        const value = bridge.answers[functions.value];
        bridge.answers[functions.value] = (...args) => { opened.stop(); return value(...args); };
        await expectAsync(opened.fullValue('/#0')).toBeRejectedWithError('The tree is closed');
    });

    it('starts no new reading of a full value once it is closed, even after a fragment refused as stale', async () => {
        const bridge = fakeWatch(['abcdefgh']);
        const tree = createWatchTree({bridge, watch: 1});
        await tree.open();
        const value = bridge.answers[functions.value];
        bridge.answers[functions.value] = (...args) => {
            if (args[2] === 0) return value(...args);  // the first fragment of an attempt
            tree.stop();                               // closed while the next fragment is awaited
            throw Object.assign(new Error('replaced'), {code: codes.stale});
        };
        const before = bridge.calls.length;
        await expectAsync(tree.fullValue('/#0')).toBeRejectedWithError('The tree is closed');
        const initial = bridge.calls.slice(before).filter(([name, , , snapshot]) => name === functions.value && snapshot === 0);
        expect(initial.length).withContext('no initial request after the closure').toBe(1);
    });

    it('tells once that its watch is gone, and sends nothing more', async () => {
        const bridge = fakeWatch(['a']);
        let gone = 0;
        const tree = createWatchTree({bridge, watch: 1, onGone: () => ++gone});
        await tree.open();
        bridge.answers[functions.changes] = () => { throw refusal(codes.unknown); };
        await expectAsync(tree.refresh()).toBeRejected();
        expect(gone).toBe(1);
        expect(tree.gone).toBeTrue();
        const before = bridge.calls.length;
        expect(await tree.refresh()).toBeFalse();
        await expectAsync(tree.expand('')).toBeRejected();
        expect(bridge.calls.length).toBe(before);
        expect(gone).toBe(1);
    });
});

describe('The view of a tree', () => {
    async function shown(values, options = {}) {
        const bridge = fakeWatch(values, options);
        const list = document.createElement('ul');
        document.body.append(list);
        let view = null;
        const tree = createWatchTree({bridge, watch: 1, onUpdate: () => view?.render()});
        view = createTreeView({tree, list, view: options.view ?? {}, name: options.name ?? null});
        await tree.open();
        return {bridge, tree, view, list};
    }
    const rows = list => [...list.querySelectorAll('li[role="treeitem"]')];
    afterEach(() => { for (const list of document.querySelectorAll('ul.tree')) list.remove(); });

    it('shows a row per node, its texts as text and never as HTML', async () => {
        const {list} = await shown(['<img src=x onerror=alert(1)>', 'b']);
        expect(rows(list).map(item => item.dataset.path)).toEqual(['', '/#0', '/#1']);
        expect(list.querySelector('img')).toBeNull();
        expect(rows(list)[1].querySelector('.value').textContent).toBe('<img src=x onerror=alert(1)>');
        expect(rows(list)[0].getAttribute('aria-expanded')).toBe('true');
        expect(rows(list)[0].querySelector('.kind').textContent).toBe('sequence [2]');
    });

    it('folds and unfolds with the toggle', async () => {
        const {list, tree} = await shown(['a', 'b']);
        list.querySelector('button.toggle').click();
        await tree.settled();
        expect(rows(list).length).toBe(1);
        expect(rows(list)[0].getAttribute('aria-expanded')).toBe('false');
        list.querySelector('button.toggle').click();
        await tree.settled();
        expect(rows(list).length).toBe(3);
    });

    it('edits a writable value in place: Enter writes, Escape cancels', async () => {
        const values = ['a', 'b'];
        const {list, tree, view} = await shown(values);
        rows(list)[2].querySelector('.value').click();
        let input = list.querySelector('input.edit');
        expect(input.value).toBe('b');
        input.value = 'cancelled';
        input.dispatchEvent(new Event('input'));
        input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}));
        expect(list.querySelector('input.edit')).toBeNull();
        expect(values[1]).toBe('b');
        view.edit('/#1');
        input = list.querySelector('input.edit');
        input.value = 'B';
        input.dispatchEvent(new Event('input'));
        // A refresh while the user types keeps the edition and its text.
        await tree.refresh();
        expect(list.querySelector('input.edit').value).toBe('B');
        list.querySelector('input.edit').dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
        await tree.settled();
        await Promise.resolve();
        expect(values[1]).toBe('B');
        expect(rows(list)[2].querySelector('.value').textContent).toBe('B');
    });

    it('writes against the value shown when the edition began, not the one a refresh read meanwhile', async () => {
        const values = ['a', 'b'];
        const {list, tree, view} = await shown(values);
        view.edit('/#1');
        const input = list.querySelector('input.edit');
        input.value = 'mine';
        input.dispatchEvent(new Event('input'));
        // The application changes the value while the user types, and a pass reads it: the tree now holds 'theirs'.
        values[1] = 'theirs';
        await tree.refresh();
        expect(tree.nodes.get('/#1').node.value).toBe('theirs');
        list.querySelector('input.edit').dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
        await tree.settled();
        await new Promise(resolve => setTimeout(resolve));
        // The user never saw 'theirs': the write is refused as stale, and the change of the application is kept.
        expect(values[1]).toBe('theirs');
        expect(rows(list)[2].querySelector('.note').textContent).toContain('read again');
        expect(rows(list)[2].querySelector('.value').textContent).toBe('theirs');
    });

    it('gives up an edition when a refresh changed what tells the node: its label, or an ancestor', async () => {
        const values = ['a', 'b'];
        const {bridge, list, tree, view} = await shown(values);
        view.edit('/#1');
        const input = list.querySelector('input.edit');
        input.value = 'mine';
        input.dispatchEvent(new Event('input'));
        // Another element takes the place, with the same value: only its label tells it (a map whose key was replaced).
        const changes = bridge.answers[functions.changes];
        bridge.answers[functions.changes] = (...args) => {
            bridge.answers[functions.changes] = changes;
            const page = changes(...args);
            return [[node('/#1', 'string', 'other key', 'b', 0, flags.writable)], 0, page[2], false];
        };
        await tree.refresh();
        const before = bridge.calls.length;
        list.querySelector('input.edit').dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
        await tree.settled();
        await new Promise(resolve => setTimeout(resolve));
        expect(bridge.calls.length).withContext('no write is sent').toBe(before);
        expect(values[1]).toBe('b');
        expect(rows(list)[2].querySelector('.note').textContent).toContain('read again');
    });

    it('keeps the position being typed, and its focus, across a refresh', async () => {
        const {list, tree} = await shown(Array.from({length: 300}, (unused, index) => `v${index}`), {pageSize: 3});
        let input = list.querySelector('input.position');
        input.focus();
        input.value = '25';
        input.dispatchEvent(new Event('input'));
        await tree.refresh();
        input = list.querySelector('input.position');
        expect(input.value).toBe('25');
        expect(document.activeElement).toBe(input);
        input.value = '250';
        input.dispatchEvent(new Event('input'));
        input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
        await tree.settled();
        expect(tree.nodes.has('/#250')).toBeTrue();
        expect(list.querySelector('input.position').value).toBe('');
    });

    it('does not load again by itself a window that failed: the user asks for it', async () => {
        const bridge = fakeWatch(Array.from({length: 7}, (unused, index) => `v${index}`), {pageSize: 3});
        const list = document.createElement('ul');
        document.body.append(list);
        const observed = [];
        class FakeObserver {
            constructor(callback) { this.callback = callback; observed.push(this); this.targets = []; }
            observe(target) { this.targets.push(target); }
            disconnect() { this.targets = []; }
            fire() { this.callback(this.targets.map(target => ({target, isIntersecting: true}))); }
        }
        let shown = null;
        const tree = createWatchTree({bridge, watch: 1, onUpdate: () => shown?.render()});
        shown = createTreeView({tree, list, view: {IntersectionObserver: FakeObserver}});
        await tree.open();
        const children = bridge.answers[functions.children];
        let refused = 0;
        bridge.answers[functions.children] = () => { ++refused; throw Object.assign(new Error('the executor has no loop'), {code: codes.unavailable}); };
        observed.at(-1).fire();
        await tree.settled();
        await new Promise(resolve => setTimeout(resolve));
        expect(refused).toBe(1);
        expect(list.querySelector('button.more').parentElement.textContent).toContain('could not be loaded');
        observed.at(-1).fire();  // the window comes into view again: nothing is asked
        await new Promise(resolve => setTimeout(resolve));
        expect(refused).toBe(1);
        bridge.answers[functions.children] = children;
        list.querySelector('button.more').click();  // the user asks
        await tree.settled();
        await new Promise(resolve => setTimeout(resolve));
        expect(tree.nodes.has('/#3')).toBeTrue();
        list.remove();
    });

    it('says why a write was not applied, beside the node', async () => {
        const {list, tree, view} = await shown(['a']);
        view.edit('/#0');
        const input = list.querySelector('input.edit');
        input.value = 'refuse';
        input.dispatchEvent(new Event('input'));
        input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
        await tree.settled();
        await new Promise(resolve => setTimeout(resolve));
        expect(rows(list)[1].querySelector('.note').textContent).toBe('not this value');
        expect(writeFailure({code: codes.stale, message: ''})).toContain('read again');
    });

    it('offers no edition of a value that is not writable', async () => {
        const {list} = await shown(['a'], {writable: false});
        rows(list)[1].querySelector('.value').click();
        expect(list.querySelector('input.edit')).toBeNull();
        expect(rows(list)[1].querySelector('.value').classList.contains('writable')).toBeFalse();
    });

    it('highlights what the last pass changed', async () => {
        const values = ['a', 'b'];
        const {list, tree} = await shown(values);
        values[0] = 'A';
        await tree.refresh();
        expect(rows(list).map(item => item.classList.contains('changed'))).toEqual([false, true, false]);
        await tree.refresh();
        expect(rows(list).some(item => item.classList.contains('changed'))).toBeFalse();
    });

    it('tells the positions that are not loaded, and loads them on demand', async () => {
        const {list, tree} = await shown(Array.from({length: 7}, (unused, index) => `v${index}`), {pageSize: 3});
        const more = list.querySelector('button.more');
        expect(more.textContent).toBe('[3] to [6] not loaded');
        more.click();
        await tree.settled();
        expect(rows(list).length).toBe(7);
        expect(list.querySelector('button.more').textContent).toBe('[6] not loaded');
    });
    it('shows the kind of elements of one kind once, on their container, and their labels in a narrow column', async () => {
        const {list} = await shown(['a', 'bb', 'ccc']);
        const group = list.querySelector('ul.tree ul.tree');
        expect(group.classList.contains('homogeneous')).toBeTrue();
        expect(group.style.getPropertyValue('--label-width')).toBe('3ch');
        expect(rows(list)[0].querySelector(':scope > .row > .of').textContent).toBe('of string');
        expect(rows(list).slice(1).map(item => item.querySelector('.kind'))).toEqual([null, null, null]);
        expect(rows(list).slice(1).map(item => item.querySelector('.label').textContent)).toEqual(['[0]', '[1]', '[2]']);
        // Strings of different lengths are no grid: no block is offered.
        expect(list.querySelector('ul.block')).toBeNull();
        expect(list.querySelector('button.layout')).toBeNull();
    });

    it('shows a container of strings of the same length as a block, and goes back to the tree with its button', async () => {
        const storage = new Map();
        const localStorage = {getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value)};
        const grid = ['#..', '.#.', '..#'];
        const {list, tree} = await shown(grid, {view: {localStorage}, name: 'grid'});
        expect(blockWidth(tree, '')).toBe(3);
        let block = list.querySelector('ul.tree.block');
        expect(block).not.toBeNull();
        // One line per element, its value only: no label, no kind.
        expect([...block.children].map(line => line.textContent)).toEqual(grid);
        expect(block.querySelector('.label, .kind')).toBeNull();
        expect([...block.children].map(line => line.dataset.path)).toEqual(['/#0', '/#1', '/#2']);
        const layout = list.querySelector('button.layout');
        expect(layout.textContent).toBe('Tree');
        layout.focus();
        layout.click();
        expect(list.querySelector('ul.block')).toBeNull();
        expect(rows(list).slice(1).map(item => item.querySelector('.label').textContent)).toEqual(['[0]', '[1]', '[2]']);
        expect(document.activeElement).withContext('the focus stays on the button').toBe(list.querySelector('button.layout'));
        expect(keptLayout({localStorage}, 'grid', '')).toBe(layouts.tree);
        // The choice is kept for this watch: another view of it opens on the tree, another watch on the block.
        const again = await shown(grid, {view: {localStorage}, name: 'grid'});
        expect(again.list.querySelector('ul.block')).toBeNull();
        const other = await shown(grid, {view: {localStorage}, name: 'other'});
        expect(other.list.querySelector('ul.block')).not.toBeNull();
        list.querySelector('button.layout').click();
        block = list.querySelector('ul.tree.block');
        expect(block).not.toBeNull();
        expect(keptLayout({localStorage}, 'grid', '')).toBe(layouts.block);
    });

    it('shows a grid of 32 by 32 characters as a square block, its rows as compact as its columns', async () => {
        const style = document.createElement('style');
        style.textContent = watchTreeStyles;
        document.head.append(style);
        try {
            const {list} = await shown(Array.from({length: 32}, (unused, row) => '.#'.repeat(16).slice(row % 2, row % 2 + 32).padEnd(32, '.')));
            const box = list.querySelector('ul.block').getBoundingClientRect();
            expect(Math.abs(box.width / box.height - 1)).withContext(`${box.width} x ${box.height}`).toBeLessThan(0.05);
            // Smaller than the rows of the tree, which a grid of 32 rows would make more than three times higher than wide.
            expect(box.height).toBeLessThan(32 * 16);
        } finally { style.remove(); }
    });

    it('keeps the names of the fields of an object, or the keys of a map, whose values have the same length', async () => {
        const bridge = fakeWatch(['abc', 'def']);
        const children = bridge.answers[functions.children];
        const named = (kind, label) => (...args) => {
            const page = children(...args);
            page[0] = page[0].map(([path, nodeKind, nodeLabel, ...rest]) => path === '' ? [path, kind, nodeLabel, ...rest] : [path, nodeKind, label(path), ...rest]);
            return page;
        };
        for (const [kind, label] of [['map', path => `key${path.slice(2)}`], ['described', path => `field${path.slice(2)}`], ['sequence', path => `#${path.slice(2)}`]]) {
            bridge.answers[functions.children] = named(kind, label);
            const list = document.createElement('ul');
            document.body.append(list);
            let view = null;
            const tree = createWatchTree({bridge, watch: 1, onUpdate: () => view?.render()});
            view = createTreeView({tree, list, view: {}});
            await tree.open();
            expect(blockWidth(tree, '')).withContext(kind).toBe(0);
            expect(list.querySelector('ul.block')).withContext(kind).toBeNull();
            expect(rows(list).slice(1).map(item => item.querySelector('.label').textContent)).toEqual([label('/#0'), label('/#1')]);
        }
    });

    it('highlights the lines of a block that the last pass changed', async () => {
        const grid = ['....', '....', '....'];
        const {list, tree} = await shown(grid);
        grid[1] = '.##.';
        await tree.refresh();
        const lines = () => [...list.querySelectorAll('ul.block > li')];
        expect(lines().map(line => line.classList.contains('changed'))).toEqual([false, true, false]);
        expect(lines()[1].textContent).toBe('.##.');
        await tree.refresh();
        expect(lines().some(line => line.classList.contains('changed'))).toBeFalse();
        // A line that no longer has the length of the others makes the block a tree again, until they agree.
        grid[2] = '..';
        await tree.refresh();
        expect(list.querySelector('ul.block')).toBeNull();
    });

    it('edits a writable line of a block in place, and reads the full value of one that is cut', async () => {
        const grid = ['ab', 'cd'];
        const {list, tree, bridge} = await shown(grid);
        list.querySelectorAll('ul.block .value')[1].click();
        const input = list.querySelector('ul.block input.edit');
        expect(input.value).toBe('cd');
        input.value = 'CD';
        input.dispatchEvent(new Event('input'));
        input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
        await tree.settled();
        await new Promise(resolve => setTimeout(resolve));
        expect(grid[1]).toBe('CD');
        expect(list.querySelectorAll('ul.block > li')[1].textContent).toBe('CD');
        // A line that C++ cut offers its full value.
        const children = bridge.answers[functions.children];
        bridge.answers[functions.children] = (...args) => {
            const page = children(...args);
            page[0] = page[0].map(([path, kind, label, value, count, bits]) => [path, kind, label, value, count, path === '/#0' ? bits | flags.truncated : bits]);
            return page;
        };
        await tree.load('', 0);
        list.querySelector('ul.block button.full').click();
        await new Promise(resolve => setTimeout(resolve));
        expect(list.querySelector('ul.block pre.full').textContent).toBe('ababab');
    });

    it('keeps a bounded number of layouts, the last chosen ones', () => {
        const storage = new Map();
        const view = {localStorage: {getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value)}};
        for (let index = 0; index <= keptLayoutCount; ++index) keepLayout(view, `watch${index}`, '', layouts.tree);
        expect(keptLayout(view, 'watch0', '')).toBeNull();
        expect(keptLayout(view, `watch${keptLayoutCount}`, '')).toBe(layouts.tree);
        expect(keptLayout({localStorage: {getItem: () => { throw new Error('blocked'); }}}, 'grid', '')).toBeNull();
        expect(keptLayout({localStorage: {getItem: () => '{"x":'}}, 'grid', '')).toBeNull();
    });
});
