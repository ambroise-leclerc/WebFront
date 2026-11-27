// Specs of the trees of the watches (#682, ADR-027 sections 3 to 6): the nodes and the pages, the order of the calls that
// change C++'s memory, the passes of differences and what interrupts them, the resynchronization, writes and full values,
// and the view that shows a tree. A fake of the C++ side answers here; the real one is read in a browser by
// test/web/suite/specs/inspector-specs.mjs.
import {flags, functions, codes, readNode, readNodePage, readLimits, writeBound, utf8Bytes, parentOf, createWatchTree, createTreeView,
    writeFailure} from '../../../js/developer/environment/inspector-watches.mjs';

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
    async function shown(values, options) {
        const bridge = fakeWatch(values, options);
        const list = document.createElement('ul');
        document.body.append(list);
        let view = null;
        const tree = createWatchTree({bridge, watch: 1, onUpdate: () => view?.render()});
        view = createTreeView({tree, list, view: {}});
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
});
