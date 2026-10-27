// ADR-003: optional DOM command interpreter. Only this module touches the DOM; WebFront.js routes command 5.
const maxBytes = 1024 * 1024;
const maxEntries = 65536;
const decoder = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true});
const defaultTags = 'div span p ul ol li button input label select option form fieldset legend h1 h2 h3 h4 table thead tbody tr td th strong em small output progress meter textarea section header footer nav a img br hr pre code';
const defaultAttributes = 'class id style title type name value placeholder min max step checked disabled readonly for href src alt role tabindex colspan rowspan width height selected lang dir hidden';
const defaultEvents = 'click dblclick input change submit keydown keyup focus blur pointerdown pointerup';
const forbiddenTags = new Set('script iframe frame frameset object embed link meta base style svg math'.split(' '));
const forbiddenAttributes = new Set('srcdoc is xmlns srcset imagesrcset ping'.split(' '));
const urlAttributes = new Set('href src action formaction poster cite background data codebase manifest longdesc usemap profile'.split(' '));
const propertyNames = new Set(['value', 'checked', 'disabled', 'indeterminate', 'selectedIndex']);
const list = text => new Set(text.split(' '));
const validName = name => /^[a-z][a-z0-9-]*$/.test(name);

function reader(bytes) {
    let offset = 6;
    return {
        get done() { return offset === bytes.length; },
        byte() {
            if (offset >= bytes.length) throw new RangeError('truncated DOM batch');
            return bytes[offset++];
        },
        uint() {
            let value = 0;
            for (let i = 0; i < 5; ++i) {
                const next = this.byte();
                if ((i === 4 && next > 15) || (i > 0 && next === 0))
                    throw new RangeError('noncanonical or overflowing DOM varint');
                value += (next & 127) * 2 ** (7 * i);
                if (!(next & 128)) return value;
            }
            throw new RangeError('overflowing DOM varint');
        },
        string() {
            const size = this.uint();
            if (size > bytes.length - offset) throw new RangeError('truncated DOM string');
            const value = decoder.decode(bytes.subarray(offset, offset + size));
            offset += size;
            return value;
        },
    };
}

/** Install once per bridge/document. The page explicitly lends empty elements with island(name, element).
 * Extension lists are copied; executable tags/attributes and unsafe URL schemes remain forbidden.
 * apply throws on rejection; the bridge reports it to C++ as wfDom.rejected(batchId, reason).
 */
export function installDomInterpreter(bridge = globalThis.webFront, extensions = {}) {
    if (bridge.domBatchHandler) throw new Error('DOM interpreter already installed');
    const tags = list(defaultTags), attributes = list(defaultAttributes), events = list(defaultEvents);
    for (const [key, target] of [['tags', tags], ['attributes', attributes], ['events', events]]) {
        for (const name of extensions[key] || []) {
            if (!validName(name) || name.startsWith('on') ||
                (key === 'tags' && forbiddenTags.has(name)) ||
                (key === 'attributes' && forbiddenAttributes.has(name)))
                throw new Error(`forbidden DOM extension ${name}`);
            target.add(name);
        }
    }
    const names = [];
    let nodes = new Map();
    const islands = new Map(), listeners = new Map();
    const allowedAttribute = name => !name.startsWith('on') && !forbiddenAttributes.has(name) &&
        (attributes.has(name) || /^(aria|data)-[a-z0-9-]+$/.test(name));
    const safeUrl = value => ['http:', 'https:', 'mailto:'].includes(new URL(value, document.baseURI).protocol);

    function unlisten(id, index) {
        const key = `${id}:${index}`, entry = listeners.get(key);
        if (entry) {
            entry.node.removeEventListener(entry.name, entry.listener);
            listeners.delete(key);
        }
    }
    function cleanListeners(live) {
        for (const [key, entry] of listeners)
            if (!live.has(entry.id)) unlisten(entry.id, Number(key.split(':')[1]));
    }
    function listen(id, index, name, flags) {
        unlisten(id, index);
        const node = nodes.get(id).node;
        const listener = event => {
            if (flags & 1) event.preventDefault();
            if (flags & 2) event.stopPropagation();
            const value = name === 'input' || name === 'change'
                ? (node.type === 'checkbox' ? String(node.checked) : String(node.value))
                : name === 'keydown' || name === 'keyup' ? event.key : '';
            bridge.cppFunction('wfDom.event')(id, index, value).catch(() => {});
        };
        node.addEventListener(name, listener);
        listeners.set(`${id}:${index}`, {id, node, name, listener});
    }

    function decode(bytes) {
        if (!(bytes instanceof Uint8Array) || bytes.length < 6 || bytes.length > maxBytes || bytes[0] !== 5 || bytes[1] !== 1)
            throw new RangeError('invalid DOM batch header or size');
        const input = reader(bytes), operations = [];
        // Retain each fully decoded NAME even if a later operation or validation rejects the batch.
        while (!input.done) {
            const op = input.byte();
            if (op < 1 || op > 13) throw new Error(`unknown DOM operation ${op}`);
            const a = input.uint();
            const b = [2, 4, 5, 6, 8, 9, 11, 12, 13].includes(op) ? input.uint() : 0;
            const c = op === 9 ? input.uint() : op === 11 ? input.byte() : 0;
            const value = [1, 3, 4, 6, 7].includes(op) ? input.string() : '';
            if (op === 1) {
                if (a !== names.length || a >= maxEntries || new TextEncoder().encode(value).length > 1024)
                    throw new Error('invalid DOM name definition');
                names.push(value);
            }
            operations.push({op, a, b, c, value, nameLimit: names.length});
        }
        return operations;
    }

    function apply(bytes) {
        const operations = decode(bytes);
        // A shadow tree tracks the state produced by every preceding operation, including removals and moves.
        const shadow = new Map(Array.from(nodes, ([id, entry]) => [id, {...entry, children: new Set(entry.children)}]));
        const steps = [];
        const get = id => {
            const entry = shadow.get(id);
            if (!entry) throw new Error(`unknown DOM node ${id}`);
            return entry;
        };
        const writable = id => {
            const entry = get(id);
            if (entry.island) throw new Error('the page owns the island element');
            return entry;
        };
        const element = id => {
            const entry = writable(id);
            if (entry.text) throw new Error('expected DOM element');
            return entry;
        };
        const fresh = (id, entry) => {
            if (!id || shadow.has(id) || shadow.size >= maxEntries) throw new Error('invalid or duplicate DOM node');
            shadow.set(id, {...entry, parent: null, children: new Set()});
        };
        const forget = id => {
            // Iterative traversal: hostile deep trees must not overflow the JavaScript stack.
            const pending = [id];
            while (pending.length) {
                const current = pending.pop(), entry = get(current);
                for (const child of entry.children) pending.push(child);
                shadow.delete(current);
            }
        };
        for (const {op, a, b, c, value, nameLimit} of operations) {
            const nameAt = index => {
                if (index >= nameLimit) throw new Error(`unknown DOM name ${index}`);
                return names[index];
            };
            switch (op) {
            case 1: break;
            case 2: {
                const tag = nameAt(b);
                if (!tags.has(tag) || forbiddenTags.has(tag)) throw new Error(`forbidden DOM tag ${tag}`);
                fresh(a, {text: false});
                steps.push(() => { nodes.set(a, {node: document.createElement(tag)}); });
                break;
            }
            case 3:
                fresh(a, {text: true});
                steps.push(() => { nodes.set(a, {node: document.createTextNode(value)}); });
                break;
            case 4: case 5: {
                element(a);
                const name = nameAt(b);
                if (!allowedAttribute(name)) throw new Error(`forbidden DOM attribute ${name}`);
                if (op === 4 && urlAttributes.has(name) && !safeUrl(value)) throw new Error('unsafe DOM URL');
                steps.push(() => op === 4 ? nodes.get(a).node.setAttribute(name, value) : nodes.get(a).node.removeAttribute(name));
                break;
            }
            case 6: {
                element(a);
                const name = nameAt(b);
                if (!propertyNames.has(name)) throw new Error(`forbidden DOM property ${name}`);
                if (['checked', 'disabled', 'indeterminate'].includes(name) && !['true', 'false'].includes(value))
                    throw new Error('invalid DOM boolean');
                if (name === 'selectedIndex' && (!/^-?\d+$/.test(value) || !Number.isInteger(Number(value)) || Number(value) < -1 || Number(value) > 2147483647))
                    throw new Error('invalid DOM selectedIndex');
                const converted = name === 'value' ? value : name === 'selectedIndex' ? Number(value) : value === 'true';
                steps.push(() => { nodes.get(a).node[name] = converted; });
                break;
            }
            case 7:
                if (!writable(a).text) throw new Error('SET_TEXT requires a text node');
                steps.push(() => { nodes.get(a).node.data = value; });
                break;
            case 8: case 9: {
                const parent = get(a), child = writable(b);
                if (parent.text) throw new Error('a text node cannot own children');
                if (op === 9 && get(c).parent !== a) throw new Error('insertBefore reference is not a child');
                for (let ancestor = a; ancestor !== null; ancestor = get(ancestor).parent)
                    if (ancestor === b) throw new Error('DOM cycle');
                if (child.parent !== null) get(child.parent).children.delete(b);
                child.parent = a;
                parent.children.add(b);
                steps.push(() => op === 8 ? nodes.get(a).node.appendChild(nodes.get(b).node) :
                    nodes.get(a).node.insertBefore(nodes.get(b).node, nodes.get(c).node));
                break;
            }
            case 10: {
                const entry = writable(a);
                if (entry.parent !== null) get(entry.parent).children.delete(a);
                const removed = [];
                const pending = [a];
                while (pending.length) {
                    const id = pending.pop();
                    removed.push(id);
                    for (const child of get(id).children) pending.push(child);
                }
                forget(a);
                steps.push(() => {
                    nodes.get(a).node.remove();
                    for (const id of removed) nodes.delete(id);
                    cleanListeners(nodes);
                });
                break;
            }
            case 11: case 12: {
                element(a);
                const name = nameAt(b);
                if (!events.has(name) || c > 3) throw new Error('invalid DOM event or flags');
                steps.push(() => op === 11 ? listen(a, b, name, c) : unlisten(a, b));
                break;
            }
            case 13: {
                const name = nameAt(b), node = islands.get(name)?.element;
                if (!node) throw new Error(`unknown DOM island ${name}`);
                if (Array.from(shadow.values()).some(entry => entry.node === node)) throw new Error('island already mounted');
                fresh(a, {node, text: false, island: true});
                steps.push(() => nodes.set(a, {node}));
                break;
            }
            }
        }
        // Residual DOM exceptions (for example a page's custom element) propagate to the bridge as a rejection.
        // In that case discard handles: the actual DOM may have been partly changed and must be lent again.
        try {
            for (const step of steps) step();
        } catch (error) {
            cleanListeners(new Map());
            nodes.clear();
            islands.clear();
            throw error;
        }
        for (const [id, entry] of shadow) entry.node = nodes.get(id).node;
        nodes = shadow;
    }

    const api = {
        apply,
        island(name, element) {
            if (!(element instanceof HTMLElement) || !tags.has(element.localName) || forbiddenTags.has(element.localName))
                throw new TypeError('an island must be an allowed HTML element');
            const previous = islands.get(name);
            if (previous && previous.element !== element) throw new Error('island name already lent');
            if (!previous) {
                if (element.childNodes.length || Array.from(islands.values()).some(entry => entry.element === element || entry.element.contains(element) || element.contains(entry.element)))
                    throw new Error('islands must be empty and disjoint');
            }
            const token = {element};
            islands.set(name, token);
            return () => {
                if (islands.get(name) !== token) return;
                islands.delete(name);
                const removed = new Set();
                for (const [id, entry] of nodes) {
                    if (entry.node === element || element.contains(entry.node)) removed.add(id);
                    else {
                        for (let parent = entry.parent; parent !== null; parent = nodes.get(parent)?.parent ?? null)
                            if (nodes.get(parent)?.node === element) { removed.add(id); break; }
                    }
                }
                for (const id of removed) nodes.delete(id);
                cleanListeners(nodes);
            };
        },
    };
    bridge.domBatchHandler = apply;
    return api;
}
