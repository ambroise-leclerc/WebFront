// ADR-018 (#378): throwaway interpreter of the mutation batches of DomPrototype.hpp, never part of the library.
// A batch is decoded and validated entirely before anything is applied: a rejected batch changes nothing in the page
// (only its NAME definitions are kept, since C++ considers them sent). Nodes are created with createElement and
// createTextNode only, never from HTML; tags, attributes, properties and events go through allowlists; URLs are checked.
// The page owns the islands: wfDom.island(name, element) lends an element to C++, which then owns its children only.
"use strict";
(function (global) {
    const Op = {name: 1, element: 2, text: 3, attribute: 4, unsetAttribute: 5, property: 6, setText: 7, append: 8,
                insertBefore: 9, remove: 10, listen: 11, unlisten: 12, mount: 13};
    const preventDefault = 1, stopPropagation = 2;

    const tags = new Set(["div", "span", "p", "ul", "ol", "li", "button", "input", "label", "select", "option", "form",
        "fieldset", "legend", "h1", "h2", "h3", "h4", "table", "thead", "tbody", "tr", "td", "th", "strong", "em",
        "small", "output", "progress", "meter", "textarea", "section", "header", "footer", "nav", "a", "img", "br", "hr",
        "pre", "code"]);
    const attributes = new Set(["class", "id", "style", "title", "type", "name", "value", "placeholder", "min", "max",
        "step", "checked", "disabled", "readonly", "for", "href", "src", "alt", "role", "tabindex", "colspan", "rowspan",
        "width", "height", "selected", "lang", "dir", "hidden"]);
    const urlAttributes = new Set(["href", "src"]);
    const properties = new Set(["value", "checked", "disabled", "indeterminate", "selectedIndex"]);
    const events = new Set(["click", "dblclick", "input", "change", "submit", "keydown", "keyup", "focus", "blur",
        "pointerdown", "pointerup"]);

    const allowedAttribute = name => attributes.has(name) || /^(aria|data)-[a-z0-9-]+$/.test(name);
    // Relative URLs, fragments, http(s) and mailto only: no javascript:, data: or other schemes.
    function safeUrl(value) {
        const scheme = /^\s*([a-zA-Z][a-zA-Z0-9+.-]*):/.exec(value);
        return !scheme || ["http", "https", "mailto"].includes(scheme[1].toLowerCase());
    }

    const decoder = new TextDecoder("utf-8", {fatal: true});
    const names = [];           // interned names, by index
    const nodes = new Map();    // id -> Node
    const ids = new WeakMap();  // Node -> id
    const listeners = new Map();  // "id:event" -> listener
    const islands = new Map();  // name -> element lent by the page
    const stats = {batches: 0, operations: 0, bytes: 0, rejected: 0, lastError: ""};

    function reader(bytes) {
        let offset = 0;
        return {
            get done() { return offset >= bytes.length; },
            byte() {
                if (offset >= bytes.length) throw new RangeError("truncated batch");
                return bytes[offset++];
            },
            varint() {
                let value = 0, shift = 0, byte;
                do {
                    if (shift > 28) throw new RangeError("varint too long");
                    byte = this.byte();
                    value += (byte & 0x7f) * 2 ** shift;
                    shift += 7;
                } while (byte & 0x80);
                return value;
            },
            string() {
                const length = this.varint();
                if (offset + length > bytes.length) throw new RangeError("truncated string");
                const start = offset;
                offset += length;
                // Short ASCII strings avoid TextDecoder, which dominates small text otherwise.
                if (length <= 32) {
                    let ascii = "";
                    for (let i = start; i < offset; ++i) {
                        if (bytes[i] > 0x7f) return decoder.decode(bytes.subarray(start, offset));
                        ascii += String.fromCharCode(bytes[i]);
                    }
                    return ascii;
                }
                return decoder.decode(bytes.subarray(start, offset));
            },
        };
    }

    // Phase 1: decode and validate everything; the result is a list of closures that cannot fail on valid state.
    function compile(bytes, pendingNames) {
        const input = reader(bytes);
        if (input.byte() !== 0) throw new Error("unknown batch version");
        const created = new Set();  // ids created in this batch
        const known = id => nodes.has(id) || created.has(id);
        const nameAt = index => {
            const value = index < names.length ? names[index] : pendingNames.get(index);
            if (value === undefined) throw new Error(`unknown name ${index}`);
            return value;
        };
        const node = id => {
            if (!known(id)) throw new Error(`unknown node ${id}`);
            return id;
        };
        const fresh = id => {
            if (id === 0 || known(id)) throw new Error(`node ${id} already exists`);
            created.add(id);
            return id;
        };
        const steps = [];
        while (!input.done) {
            const op = input.byte();
            switch (op) {
                case Op.name: {
                    const index = input.varint(), value = input.string();
                    if (index !== names.length + pendingNames.size) throw new Error(`name ${index} out of order`);
                    pendingNames.set(index, value);
                    break;
                }
                case Op.element: {
                    const id = fresh(input.varint()), tag = nameAt(input.varint());
                    if (!tags.has(tag)) throw new Error(`element <${tag}> is not allowed`);
                    steps.push(() => register(id, document.createElement(tag)));
                    break;
                }
                case Op.text: {
                    const id = fresh(input.varint()), value = input.string();
                    steps.push(() => register(id, document.createTextNode(value)));
                    break;
                }
                case Op.attribute: {
                    const id = node(input.varint()), name = nameAt(input.varint()), value = input.string();
                    if (!allowedAttribute(name)) throw new Error(`attribute ${name} is not allowed`);
                    if (urlAttributes.has(name) && !safeUrl(value)) throw new Error(`unsafe URL in ${name}`);
                    steps.push(() => nodes.get(id).setAttribute(name, value));
                    break;
                }
                case Op.unsetAttribute: {
                    const id = node(input.varint()), name = nameAt(input.varint());
                    steps.push(() => nodes.get(id).removeAttribute(name));
                    break;
                }
                case Op.property: {
                    const id = node(input.varint()), name = nameAt(input.varint()), value = input.string();
                    if (!properties.has(name)) throw new Error(`property ${name} is not allowed`);
                    const converted = name === "value" ? value : name === "selectedIndex" ? Number(value) : value === "true";
                    steps.push(() => { nodes.get(id)[name] = converted; });
                    break;
                }
                case Op.setText: {
                    const id = node(input.varint()), value = input.string();
                    steps.push(() => setText(id, value));
                    break;
                }
                case Op.append: {
                    const parent = node(input.varint()), child = node(input.varint());
                    steps.push(() => nodes.get(parent).appendChild(nodes.get(child)));
                    break;
                }
                case Op.insertBefore: {
                    const parent = node(input.varint()), child = node(input.varint()), reference = node(input.varint());
                    steps.push(() => nodes.get(parent).insertBefore(nodes.get(child), nodes.get(reference)));
                    break;
                }
                case Op.remove: {
                    const id = node(input.varint());
                    steps.push(() => removeNode(id));
                    break;
                }
                case Op.listen: {
                    const id = node(input.varint()), index = input.varint(), name = nameAt(index), flags = input.byte();
                    if (!events.has(name)) throw new Error(`event ${name} is not allowed`);
                    steps.push(() => listen(id, index, name, flags));
                    break;
                }
                case Op.unlisten: {
                    const id = node(input.varint()), index = input.varint();
                    nameAt(index);
                    steps.push(() => unlisten(id, index));
                    break;
                }
                case Op.mount: {
                    const id = fresh(input.varint()), name = nameAt(input.varint());
                    steps.push(() => {
                        const element = islands.get(name);
                        if (!element) throw new Error(`no island ${name}`);
                        register(id, element);
                    });
                    if (!islands.has(name)) throw new Error(`no island ${name}`);
                    break;
                }
                default:
                    throw new Error(`unknown operation ${op}`);
            }
            ++stats.operations;
        }
        return steps;
    }

    function register(id, element) {
        nodes.set(id, element);
        ids.set(element, id);
    }

    function forget(element) {
        const id = ids.get(element);
        if (id !== undefined) {
            nodes.delete(id);
            for (const key of [...listeners.keys()])
                if (key.startsWith(id + ":")) {
                    const {name, listener} = listeners.get(key);
                    element.removeEventListener(name, listener);
                    listeners.delete(key);
                }
        }
        for (const child of element.childNodes) forget(child);
    }

    // A text node keeps its identity; an element's children are replaced, so their identifiers are forgotten first.
    function setText(id, value) {
        const target = nodes.get(id);
        if (target.nodeType === Node.TEXT_NODE) {
            target.data = value;
            return;
        }
        for (const child of target.childNodes) forget(child);
        target.textContent = value;
    }

    function removeNode(id) {
        const element = nodes.get(id);
        const island = [...islands.values()].includes(element);
        if (island) {
            // An island stays the page's element: C++ only gives up its children.
            for (const child of [...element.childNodes]) { forget(child); child.remove(); }
            nodes.delete(id);
            return;
        }
        forget(element);
        element.remove();
    }

    function eventValue(event) {
        const target = event.currentTarget;
        if (event.type === "input" || event.type === "change")
            return target.type === "checkbox" ? String(target.checked) : String(target.value);
        if (event.type === "keydown" || event.type === "keyup") return event.key;
        return "";
    }

    function listen(id, index, name, flags) {
        unlisten(id, index);
        const listener = event => {
            if (flags & preventDefault) event.preventDefault();
            if (flags & stopPropagation) event.stopPropagation();
            // A Promise of the bridge: a rejection (disconnected, unknown callback) is reported, never unhandled.
            global.webFront.cppFunction("wfDom.event")(id, index, eventValue(event)).catch(error => {
                stats.lastError = String(error);
            });
        };
        nodes.get(id).addEventListener(name, listener);
        listeners.set(id + ":" + index, {name, listener});
    }

    function unlisten(id, index) {
        const key = id + ":" + index, entry = listeners.get(key);
        if (entry) {
            nodes.get(id)?.removeEventListener(entry.name, entry.listener);
            listeners.delete(key);
        }
    }

    const observers = new Set();

    global.wfDom = {
        // Called by C++ (ui.jsFunction("wfDom.apply")(bytes)): fire-and-forget, so a rejection is reported to C++.
        apply(bytes) {
            const start = performance.now();
            const pendingNames = new Map();
            let steps;
            try {
                steps = compile(bytes, pendingNames);
            } catch (error) {
                for (const [, value] of pendingNames) names.push(value);
                ++stats.rejected;
                stats.lastError = String(error.message || error);
                global.webFront.cppFunction("wfDom.rejected")(stats.lastError).catch(() => {});
                for (const observer of observers) observer({rejected: stats.lastError});
                return;
            }
            for (const [, value] of pendingNames) names.push(value);
            for (const step of steps) step();
            ++stats.batches;
            stats.bytes += bytes.length;
            const elapsed = performance.now() - start;
            for (const observer of observers) observer({bytes: bytes.length, steps: steps.length, elapsed});
        },
        // One read answered with a value: the coroutine prototype awaits it from C++ (one round trip per call).
        read(id) {
            const element = nodes.get(id);
            if (!element) throw new Error(`unknown node ${id}`);
            return element.textContent;
        },
        // The same reads, batched: the identifiers go as a Uint32Array and the texts come back joined.
        readMany(identifiers) {
            return Array.from(identifiers, id => nodes.get(id)?.textContent ?? "").join("\n");
        },
        // The "proxy" prototype: one message per operation instead of a batch.
        setTextDirect(id, value) {
            setText(id, value);
        },
        // Lends an element to C++; the returned function takes it back (React unmount). C++ owns its children only.
        island(name, element) {
            islands.set(name, element);
            return () => {
                if (islands.get(name) === element) islands.delete(name);
            };
        },
        observe(observer) {
            observers.add(observer);
            return () => observers.delete(observer);
        },
        stats,
        get nodeCount() { return nodes.size; },
    };
})(globalThis);
