// The trees of the watches (#682, ADR-027 sections 3 to 6): the content of an object or a container that the application
// declared with app.watch, unfolded level by level and refreshed while the interface is used. C++ keeps what it sent to this
// connection; the page reads pages of children (webfront.inspector.children), passes of differences (changes), full values by
// fragments (value), and writes the values the application made writable (write).
//
// One rule orders the calls: a call that changes C++'s memory of a watch (children, fold, write, the first page of a pass)
// is sent only after the answer of the previous one, since each answer carries the revision the next call must give. The
// tree therefore keeps a queue per watch. A pass never makes the user wait: an unfolding, a folding or a write goes first,
// and C++ interrupts the pass when it receives it. Reads of a full value change nothing and are sent at any time.
//
// Every text shown comes from the application: it is set as text, never as HTML.

/** The bits of a node's flags (ADR-027, section 3). */
export const flags = Object.freeze({writable: 1, truncated: 2, removed: 4, depthReached: 8, childrenChanged: 16});

export const functions = Object.freeze({
    children: 'webfront.inspector.children', changes: 'webfront.inspector.changes', value: 'webfront.inspector.value',
    fold: 'webfront.inspector.fold', reset: 'webfront.inspector.reset', write: 'webfront.inspector.write', limits: 'webfront.inspector.limits',
});

/** The codes of the refusals, as C++ sends them and as this page gives them to a text it refuses itself. */
export const codes = Object.freeze({
    unknown: 'webfront.inspector.unknown', stale: 'webfront.inspector.stale', tooLarge: 'webfront.inspector.tooLarge',
    limit: 'webfront.inspector.limit', refused: 'webfront.inspector.refused', parse: 'webfront.inspector.parse',
    readOnly: 'webfront.inspector.readOnly', unavailable: 'webfront.inspector.unavailable',
});

/** The children asked by a page of a list, and the nodes of a pass (ADR-027: 200 by default). */
export const pageCount = 200;

const whole = value => Number.isSafeInteger(value) && value >= 0;

/** A node of a page as an object; throws when it is not one. */
export function readNode(tuple) {
    if (!Array.isArray(tuple) || tuple.length !== 6) throw new Error('The inspector answered a node that is not one');
    const [path, kind, label, value, children, bits] = tuple;
    if (![path, kind, label, value].every(text => typeof text === 'string') || !whole(children) || !whole(bits))
        throw new Error('The inspector answered a node that is not one');
    return {path, kind, label, value, children, flags: bits};
}

/** A page of nodes: {nodes, next, revision, resync}; throws when it is not one. */
export function readNodePage(answer) {
    if (!Array.isArray(answer) || answer.length !== 4) throw new Error('The inspector answered a page that is not one');
    const [nodes, next, revision, resync] = answer;
    if (!Array.isArray(nodes) || !whole(next) || !whole(revision) || typeof resync !== 'boolean')
        throw new Error('The inspector answered a page that is not one');
    return {nodes: nodes.map(readNode), next, revision, resync};
}

/** What webfront.inspector.limits answers, as an object. */
export function readLimits(answer) {
    if (!Array.isArray(answer) || answer.length < 6 || !answer.every(whole)) throw new Error('The inspector answered limits that are not');
    const [maxWriteTextBytes, responseBudget, maxNodes, maxSnapshotBytes, maxPageCount, defaultPageCount] = answer;
    return {maxWriteTextBytes, responseBudget, maxNodes, maxSnapshotBytes, maxPageCount, defaultPageCount};
}

export const parentOf = path => path.slice(0, path.lastIndexOf('/'));
const under = (path, parent) => path.startsWith(`${parent}/`);

/** The bytes of a text once encoded as UTF-8, which is what the bounds count. */
export const utf8Bytes = text => new TextEncoder().encode(text).length;

/**
 * The longest text a write may carry: what C++ accepts, and what this page's own codec lets a message hold once the other
 * arguments of the request are at their largest (ADR-027, "Budget d'une requête"). `maxMessageBytes` is the codec's bound.
 */
export function writeBound(limits, maxMessageBytes = Infinity) {
    const overhead = 8 + 26 + 9 + 9 + 1027 + 259 + 5;
    return Math.max(0, Math.min(limits?.maxWriteTextBytes ?? 0, maxMessageBytes - overhead));
}

/**
 * The tree of one watch for this connection.
 *
 * `nodes` maps a path to {node, positions (Map of position to path, the children loaded), changed (the pass that last
 * changed it)}; `expanded` holds the paths the user unfolded, which survive a resynchronization. `onUpdate()` is called
 * after each change of the model, `onGone()` once C++ says that the watch no longer exists.
 */
export function createWatchTree({bridge, watch, count = pageCount, onUpdate = () => {}, onGone = () => {}, maxMessageBytes = Infinity}) {
    const call = name => bridge.cppFunction(name);
    const nodes = new Map();
    const expanded = new Set();
    let revision = 0, limits = null, gone = false, stopped = false, passes = 0, problem = null;
    // The calls that change C++'s memory, sent one at a time. A pass in progress stops when one is waiting.
    const waiting = [];
    let running = null;

    /** Rejects what was waiting: its callers are told, none is left pending. */
    const abandon = () => {
        for (const {reject} of waiting.splice(0)) reject(Object.assign(new Error('The watch is gone'), {code: codes.unknown}));
    };
    const fail = error => {
        if (error?.code === codes.unknown && !gone) {
            gone = true;
            abandon();
            onGone();
        }
        throw error;
    };
    const enqueue = work => new Promise((resolve, reject) => {
        waiting.push({work, resolve, reject});
        drain();
    });
    function drain() {
        if (running || !waiting.length) return;
        const {work, resolve, reject} = waiting.shift();
        running = (async () => {
            if (gone || stopped) throw Object.assign(new Error('The watch is gone'), {code: codes.unknown});
            return work();
        })().then(
            // Free before the caller goes on: what it asks next, having awaited this answer, is not "busy" for it.
            value => { running = null; resolve(value); },
            error => { running = null; reject(error); },
        ).finally(drain);
    }

    const forget = path => {
        for (const key of [...nodes.keys()]) if (under(key, path)) nodes.delete(key);
        const kept = nodes.get(path);
        if (kept) kept.positions = new Map();
    };
    const remove = path => {
        forget(path);
        nodes.delete(path);
        if (path !== '') {
            const parent = nodes.get(parentOf(path));
            if (parent) for (const [position, child] of parent.positions) if (child === path) parent.positions.delete(position);
        }
    };
    const keep = (node, changed = 0) => {
        const kept = nodes.get(node.path);
        if (kept) {
            if (changed && (kept.node.value !== node.value || kept.node.children !== node.children || kept.node.kind !== node.kind)) kept.changed = changed;
            kept.node = {...node, flags: node.flags & ~flags.childrenChanged};
            return kept;
        }
        const entry = {node: {...node, flags: node.flags & ~flags.childrenChanged}, positions: new Map(), changed};
        nodes.set(node.path, entry);
        return entry;
    };

    /** C++ emptied its memory: everything shown is thrown away, and read again from the root with the paths that were unfolded. */
    async function resynchronize(answered) {
        revision = answered;
        nodes.clear();
        if (!stopped) await list('', 0);
    }

    /** One page of the children of `path` from `start`, then the unfolded children that came back. Runs inside the queue. */
    async function list(path, start) {
        // A tree stopped while a call was awaited reads nothing more: it would change C++'s memory of a closed panel.
        if (stopped) return;
        let page;
        try { page = readNodePage(await call(functions.children)(watch, path, revision, start, count)); }
        catch (error) {
            // A node that is no longer in the memory, or too deep, stays folded; the tree goes on.
            if (error?.code === codes.unknown && path !== '') { expanded.delete(path); return; }
            if (error?.code === codes.limit) { problem = error.message; expanded.delete(path); return; }
            return fail(error);
        }
        if (page.resync) return resynchronize(page.revision);
        revision = page.revision;
        const [parent, ...children] = page.nodes;
        if (!parent) return;
        if (parent.flags & flags.removed) { remove(parent.path); expanded.delete(parent.path); return; }
        const entry = keep(parent);
        if (parent.flags & flags.childrenChanged) {
            // The list changed while it was read: it starts again from its first position.
            forget(path);
            return list(path, 0);
        }
        if (start === 0) for (const position of [...entry.positions.keys()]) if (position >= parent.children) remove(entry.positions.get(position));
        // The whole page first, then what it replaced and no position names any more: two keyed elements that swapped
        // their places are both kept.
        const replaced = [];
        children.forEach((child, index) => {
            const position = start + index;
            const previous = entry.positions.get(position);
            if (previous !== undefined && previous !== child.path) replaced.push(previous);
            entry.positions.set(position, child.path);
            keep(child);
        });
        const named = new Set(entry.positions.values());
        for (const previous of replaced) if (!named.has(previous)) remove(previous);
        entry.next = page.next;
        onUpdate();
        for (const child of children)
            if (expanded.has(child.path) && nodes.get(child.path)?.positions.size === 0 && child.children > 0 && !(child.flags & flags.depthReached))
                await list(child.path, 0);
    }

    /** One pass of differences. It stops as soon as another call waits: C++ interrupts it then, and the next pass finds the rest. */
    async function pass() {
        const stamp = ++passes;
        let cursor = 0;
        do {
            const page = readNodePage(await call(functions.changes)(watch, revision, cursor, count).catch(fail));
            if (page.resync) return resynchronize(page.revision);
            revision = page.revision;
            const reload = [];
            for (const node of page.nodes) {
                if (node.flags & flags.removed) { remove(node.path); continue; }
                if (!nodes.has(node.path)) continue;
                const entry = keep(node, stamp);
                if (node.flags & flags.childrenChanged) {
                    entry.changed = stamp;
                    forget(node.path);
                    if (expanded.has(node.path)) reload.push(node.path);
                }
            }
            cursor = page.next;
            if (page.nodes.length) onUpdate();
            // Reading a list again changes the memory: it interrupts the pass, whose remaining differences the next one finds.
            for (const path of reload) waiting.unshift({work: () => list(path, 0), resolve() {}, reject() {}});
        } while (cursor !== 0 && !waiting.length && !stopped);
    }

    const tree = {
        watch,
        nodes,
        expanded,
        get revision() { return revision; },
        get limits() { return limits; },
        get gone() { return gone; },
        /** The pass that ran last: a node whose `changed` equals it changed in that pass. */
        get pass() { return passes; },
        /** Why the last unfolding was refused (memory full, node too deep), until the next one succeeds. */
        get problem() { return problem; },
        /** Whether a call is running or waiting. */
        get busy() { return running !== null || waiting.length > 0; },
        /** Resolves once every call asked so far is answered. */
        async settled() { while (running || waiting.length) await (running ?? Promise.resolve()).catch(() => {}); },

        /**
         * What is shown of a node and of each of its ancestors (kind, label, value, number of children), as one text; null
         * when one of them is no longer shown. An edition keeps it, and is given up when it changed meanwhile.
         */
        shown(path) {
            const parts = [];
            for (let at = path; ; at = parentOf(at)) {
                const entry = nodes.get(at);
                if (!entry) return null;
                parts.push([entry.node.kind, entry.node.label, entry.node.value, entry.node.children].join('\u0000'));
                if (at === '') return parts.join('\u0001');
            }
        },
        /** Opens the watch: C++'s memory of it is emptied, then the root and its first children are read. */
        open() {
            return enqueue(async () => {
                limits ??= readLimits(await call(functions.limits)());
                revision = await call(functions.reset)(watch).catch(fail);
                nodes.clear();
                expanded.add('');
                await list('', 0);
                onUpdate();
            });
        },
        /** Unfolds a node: its first page of children. */
        expand(path) {
            expanded.add(path);
            problem = null;
            return enqueue(async () => { await list(path, 0); onUpdate(); });
        },
        /** Reads the children of `path` from `start`: the next page of a list, or a window anywhere in a large container. */
        load(path, start) {
            return enqueue(async () => { await list(path, start); onUpdate(); });
        },
        /** Folds a node: C++ forgets its descendants, and so does the page. */
        collapse(path) {
            expanded.delete(path);
            for (const key of [...expanded]) if (under(key, path)) expanded.delete(key);
            return enqueue(async () => {
                if (!nodes.has(path)) return;
                const page = readNodePage(await call(functions.fold)(watch, path, revision).catch(fail));
                if (page.resync) await resynchronize(page.revision);
                else {
                    revision = page.revision;
                    forget(path);
                }
                onUpdate();
            });
        },
        /** A pass of differences, unless a call is already running or waiting: the period only reads when nothing else does. */
        refresh() {
            if (tree.busy || gone || stopped || !nodes.size) return Promise.resolve(false);
            return enqueue(async () => { await pass(); onUpdate(); return true; });
        },
        /**
         * Writes `text` into the node. Resolves with {code, message}: an empty code when the value was written, else why it
         * was not. A text longer than the bound is refused here, without a call. After a `stale` refusal the node is read again.
         * `expected` is the value the user saw when the edition began; without it, the value the tree holds now. A pass may
         * have refreshed the tree since: C++ must compare the object with what was displayed, not with what was read last.
         * `shown` (tree.shown(path) when the edition began) is checked again just before the write is sent.
         */
        write(path, text, expected = undefined, shown = undefined) {
            const bound = writeBound(limits, maxMessageBytes);
            if (utf8Bytes(text) > bound)
                return Promise.resolve({code: codes.tooLarge, message: `The text is longer than ${bound} bytes`});
            return enqueue(async () => {
                const entry = nodes.get(path);
                if (!entry) return {code: codes.unknown, message: 'The node is no longer shown'};
                // A pass answered while the write waited may have changed what the edition was started on.
                if (shown !== undefined && tree.shown(path) !== shown) return {code: codes.stale, message: ''};
                let answer;
                try { answer = await call(functions.write)(watch, revision, path, expected ?? entry.node.value, text); }
                catch (error) {
                    if (error?.code === codes.unknown) return fail(error);
                    // Rejected before any effect: nothing changed, the revision included. A stale revision is cured by reading again.
                    if (error?.code === codes.stale) await list(path, 0);
                    onUpdate();
                    return {code: error?.code ?? 'webfront.error', message: error?.message ?? String(error)};
                }
                const [answered, code, message] = answer;
                revision = answered;
                // Written or not, what the node shows now is read again: the parent of a leaf gives it with its siblings' state.
                // A reading that fails does not undo what C++ answered: the write is not to be sent again.
                let refresh = '';
                try { await list(path, 0); }
                catch (error) { refresh = error?.message ?? String(error); }
                onUpdate();
                return refresh ? {code, message, refresh} : {code, message};
            });
        },
        /**
         * The whole value of a node, read by fragments of one snapshot: {text, total, capped}. A reading that another one
         * replaced starts again, a few times at most.
         */
        async fullValue(path, {attempts = 3} = {}) {
            for (let attempt = 0; ; ++attempt) {
                try {
                    let [part, total, next, capped, snapshot] = await call(functions.value)(watch, path, 0, 0);
                    let text = part;
                    while (next !== 0) {
                        [part, total, next, capped] = await call(functions.value)(watch, path, snapshot, next);
                        text += part;
                    }
                    return {text, total, capped};
                } catch (error) {
                    if (error?.code !== codes.stale || attempt + 1 >= attempts) throw error;
                }
            }
        },
        /** Stops the tree: nothing more is sent. C++ frees its memory when the connection closes, or at the next open. */
        stop() {
            stopped = true;
            abandon();
        },
    };
    return tree;
}

// --- Rendering -------------------------------------------------------------------------------------------------

export const watchTreeStyles = `
ul.watches li{display:block}
ul.watches .head{display:grid;grid-template-columns:auto 1fr auto;gap:2px 12px;align-items:baseline}
ul.watches .head .location{grid-column:2/4}
ul.watches button.toggle,ul.tree button.toggle{width:24px;min-height:24px;padding:0;border:0;background:none;color:#a9b3c4;font:inherit;cursor:pointer}
ul.watches button.toggle:focus-visible,ul.tree button:focus-visible,ul.tree input:focus-visible{outline:2px solid #d7fa85;outline-offset:1px}
ul.tree{list-style:none;margin:4px 0 0;padding:0 0 0 16px;font:12px ui-monospace,monospace}
ul.tree li{padding:1px 0;border:0}
ul.tree .row{display:flex;gap:6px;align-items:baseline;min-height:24px}
ul.tree .spacer{display:inline-block;width:24px}
ul.tree .label{color:#8fd0ff}
ul.tree .kind{color:#a9b3c4;justify-self:auto}
ul.tree .value{overflow-wrap:anywhere;white-space:pre-wrap}
ul.tree .value.writable{cursor:text;border-bottom:1px dotted #46536a}
ul.tree .changed>.row .value,ul.tree .changed>.row .kind{background:#4a3d12}
ul.tree .note{color:#ffbd2e}
ul.tree input{min-width:12ch;border:1px solid #46536a;border-radius:3px;background:#141821;color:inherit;font:inherit}
ul.tree button.more,ul.tree button.full{min-height:24px;padding:0 6px;border:1px solid #46536a;border-radius:3px;background:#2f3a50;color:inherit;font:inherit;cursor:pointer}
ul.tree pre.full{max-height:240px;overflow:auto;margin:2px 0;padding:4px;background:#0f131b;white-space:pre-wrap;overflow-wrap:anywhere}`;

/** The most characters of a full value that the page shows; the rest is told by its size. */
export const shownValueCharacters = 64 * 1024;

const reasons = {
    [codes.stale]: 'The value changed since it was shown: it was read again.',
    [codes.readOnly]: 'This value is not writable.',
    [codes.limit]: 'The inspector\'s memory is full, or the node is too deep: fold some nodes.',
};

/** Why a write was not applied, for the user. */
export const writeFailure = ({code, message}) => message || reasons[code] || code;

/**
 * Shows a tree in `list` (a `ul`), and wires its controls: unfolding and folding, the next page of a list, a position in a
 * large container, the edition in place of a writable value (Enter writes, Escape cancels), and the full value of one that
 * is cut. `render()` draws the model again; it keeps the focus and an edition in progress.
 */
export function createTreeView({tree, list, view = globalThis}) {
    const page = list.ownerDocument;
    const notes = new Map();  // path, why the last write was not applied
    const full = new Map();   // path, the full value read
    let editing = null;       // {path, text}: kept across the renderings that a refresh causes
    const positions = new Map();  // path, the position being typed in its "go to" field
    list.className = 'tree';
    list.setAttribute('role', 'tree');

    const element = (tag, className, text) => {
        const made = page.createElement(tag);
        if (className) made.className = className;
        if (text !== undefined) made.textContent = text;
        return made;
    };
    const button = (className, text, label, act) => {
        const made = element('button', className, text);
        made.type = 'button';
        made.setAttribute('aria-label', label);
        made.addEventListener('click', act);
        return made;
    };
    const report = work => Promise.resolve(work).catch(() => {}).finally(render);
    // The windows that failed to load, by "path@from": they are loaded again only when the user asks, never as they come
    // into view, which would ask again and again.
    const failedLoads = new Map();
    const load = (path, from) => {
        const key = `${path}@${from}`;
        failedLoads.delete(key);
        return Promise.resolve(tree.load(path, from))
            .catch(error => { failedLoads.set(key, error?.message ?? String(error)); })
            .finally(render);
    };

    function edit(path) {
        const entry = tree.nodes.get(path);
        if (!entry || !(entry.node.flags & flags.writable)) return;
        // What the user sees now, whatever is read later: the value, and what tells the node and its ancestors.
        editing = {path, text: entry.node.value, expected: entry.node.value, shown: tree.shown(path)};
        notes.delete(path);
        render();
        list.querySelector('input.edit')?.select();
    }
    async function commit() {
        const {path, text, expected, shown} = editing;
        editing = null;
        // A refresh changed what the edition was started on (another key under the same rank, a list that grew): the text
        // was typed for what is no longer there.
        if (tree.shown(path) !== shown) {
            notes.set(path, writeFailure({code: codes.stale, message: ''}));
            render();
            return;
        }
        const answer = await tree.write(path, text, expected, shown).catch(error => ({code: error?.code ?? 'webfront.error', message: error?.message ?? String(error)}));
        if (answer.code) notes.set(path, writeFailure(answer));
        else if (answer.refresh) notes.set(path, `Written; the value could not be read again: ${answer.refresh}`);
        render();
    }

    function row(path, depth) {
        const entry = tree.nodes.get(path);
        const {node} = entry;
        const item = element('li');
        item.setAttribute('role', 'treeitem');
        item.dataset.path = path;
        if (entry.changed && entry.changed === tree.pass) item.classList.add('changed');
        const line = element('div', 'row');
        const unfolds = node.children > 0 && !(node.flags & flags.depthReached);
        const open = unfolds && tree.expanded.has(path);
        if (unfolds) {
            item.setAttribute('aria-expanded', String(open));
            line.append(button('toggle', open ? '▾' : '▸', `${open ? 'Fold' : 'Unfold'} ${node.label}`,
                () => report(open ? tree.collapse(path) : tree.expand(path))));
        } else line.append(element('span', 'spacer'));
        line.append(element('span', 'label', node.label), element('span', 'kind', node.children > 0 ? `${node.kind} [${node.children}]` : node.kind));
        if (editing?.path === path) {
            const input = element('input', 'edit');
            input.value = editing.text;
            input.setAttribute('aria-label', `New value of ${node.label}`);
            input.addEventListener('input', () => { editing.text = input.value; });
            input.addEventListener('keydown', event => {
                if (event.key === 'Enter') { event.preventDefault(); commit(); }
                else if (event.key === 'Escape') { event.preventDefault(); editing = null; render(); }
            });
            line.append(input);
        } else if (node.value !== '' || node.children === 0) {
            const value = element('span', 'value', node.value);
            if (node.flags & flags.writable) {
                value.classList.add('writable');
                value.tabIndex = 0;
                value.setAttribute('role', 'button');
                value.setAttribute('aria-label', `Edit ${node.label}: ${node.value}`);
                value.addEventListener('click', () => edit(path));
                value.addEventListener('keydown', event => { if (event.key === 'Enter' || event.key === 'F2') { event.preventDefault(); edit(path); } });
            }
            line.append(value);
        }
        if (node.flags & flags.truncated)
            line.append(button('full', 'Full value', `Read the full value of ${node.label}`, async () => {
                try { full.set(path, await tree.fullValue(path)); }
                catch (error) { notes.set(path, `The full value cannot be read: ${error?.message ?? error}`); }
                render();
            }));
        if (node.flags & flags.depthReached) line.append(element('span', 'note', 'too deep to be unfolded'));
        if (notes.has(path)) line.append(element('span', 'note', notes.get(path)));
        item.append(line);
        if (full.has(path)) {
            const {text, total, capped} = full.get(path);
            const shown = text.length > shownValueCharacters ? `${text.slice(0, shownValueCharacters)}…` : text;
            item.append(element('pre', 'full', shown),
                element('span', 'note', `${total} bytes${capped ? ', cut at the inspector\'s limit' : ''}`));
        }
        if (open) {
            const group = element('ul', 'tree');
            group.setAttribute('role', 'group');
            let expected = 0;
            const gap = (from, to) => {
                const more = element('li');
                const key = `${path}@${from}`;
                const loader = button('more', from === to - 1 ? `[${from}] not loaded` : `[${from}] to [${to - 1}] not loaded`,
                    `Load the children of ${node.label} from ${from}`, () => load(path, from));
                loader.dataset.load = key;
                more.append(loader);
                if (failedLoads.has(key)) more.append(element('span', 'note', `could not be loaded: ${failedLoads.get(key)}`));
                more.dataset.from = String(from);
                return more;
            };
            for (const [position, child] of [...entry.positions].sort((a, b) => a[0] - b[0])) {
                if (position > expected) group.append(gap(expected, position));
                if (tree.nodes.has(child)) group.append(row(child, depth + 1));
                expected = position + 1;
            }
            if (expected < node.children) {
                group.append(gap(expected, node.children));
                // A large container: any position is read directly.
                if (node.children > tree.limits?.defaultPageCount) {
                    const jump = element('li');
                    const input = element('input', 'position');
                    input.type = 'number';
                    input.min = '0';
                    input.max = String(node.children - 1);
                    input.placeholder = 'position';
                    input.setAttribute('aria-label', `Go to a position in ${node.label}`);
                    input.value = positions.get(path) ?? '';
                    input.addEventListener('input', () => positions.set(path, input.value));
                    input.addEventListener('keydown', event => {
                        const position = Number(input.value);
                        if (event.key === 'Enter' && input.value !== '' && Number.isSafeInteger(position) && position >= 0 && position < node.children) {
                            positions.delete(path);
                            load(path, position);
                        }
                    });
                    jump.dataset.jump = path;
                    jump.append(input);
                    group.append(jump);
                }
            }
            item.append(group);
        }
        return item;
    }

    function render() {
        const focused = page.activeElement;
        const within = focused && list.contains(focused);
        const where = within
            ? {path: focused.closest('li')?.dataset.path, jump: focused.closest('li')?.dataset.jump, name: focused.className.split(' ')[0]} : null;
        for (const path of [...positions.keys()]) if (!tree.nodes.has(path)) positions.delete(path);
        for (const path of [...notes.keys()]) if (!tree.nodes.has(path)) notes.delete(path);
        for (const path of [...full.keys()]) if (!tree.nodes.has(path)) full.delete(path);
        if (editing && !tree.nodes.has(editing.path)) editing = null;
        list.replaceChildren(...(tree.nodes.has('') ? [row('', 0)] : []));
        if (tree.problem) list.append(element('li', 'note', tree.problem));
        if (where?.jump !== undefined)
            [...list.querySelectorAll('li')].find(candidate => candidate.dataset.jump === where.jump)?.querySelector('input')?.focus();
        else if (where?.path !== undefined) {
            const item = [...list.querySelectorAll('li')].find(candidate => candidate.dataset.path === where.path);
            const again = item?.querySelector(`:scope > .row .${where.name || 'toggle'}`);
            again?.focus();
            if (again?.tagName === 'INPUT' && editing) again.setSelectionRange(again.value.length, again.value.length);
        }
        // The positions that are not loaded are read when they come into view.
        if (typeof view.IntersectionObserver === 'function') {
            observer?.disconnect();
            observer = new view.IntersectionObserver(entries => {
                for (const entry of entries)
                    if (entry.isIntersecting && !tree.busy && !failedLoads.has(entry.target.dataset.load)) entry.target.click();
            });
            for (const more of list.querySelectorAll('button.more')) observer.observe(more);
        }
    }
    let observer = null;

    return {render, edit, get editing() { return editing; }, stop() { observer?.disconnect(); }};
}
