// This suite uses the actual embedded module, not a bundled copy, on every selected frontend.
const moduleUrl = '/webfront-dom.mjs';
const encoder = new TextEncoder();
function uint(value) {
    const bytes = [];
    do { const low = value % 128; value = Math.floor(value / 128); bytes.push(low | (value ? 128 : 0)); } while (value);
    return bytes;
}
function str(value) { const bytes = encoder.encode(value); return [...uint(bytes.length), ...bytes]; }
function packet(ops, id = 1) {
    return new Uint8Array([5, 1, id & 255, (id >>> 8) & 255, (id >>> 16) & 255, id >>> 24, ...ops.flat()]);
}
const name = (id, value) => [1, ...uint(id), ...str(value)];
const text = (id, value) => [3, ...uint(id), ...str(value)];
const attr = (id, index, value) => [4, ...uint(id), ...uint(index), ...str(value)];
const prop = (id, index, value) => [6, ...uint(id), ...uint(index), ...str(value)];

describe('Embedded DOM command interpreter', () => {
    let install, bridge, dom, root, release;
    beforeAll(async () => { ({installDomInterpreter: install} = await import(moduleUrl)); });
    beforeEach(() => {
        bridge = {cppFunction: jasmine.createSpy('callback').and.returnValue(() => Promise.resolve())};
        dom = install(bridge);
        root = document.body.appendChild(document.createElement('div'));
        release = dom.island('root', root);
        dom.apply(packet([name(0, 'root'), name(1, 'div'), name(2, 'title'), [13, 1, 0]]));
    });
    afterEach(() => { release(); root.remove(); });

    it('creates, inserts, moves, removes and updates text without parsing HTML', () => {
        dom.apply(packet([[2, 2, 1], text(3, '<b>été 🌍</b>'), [8, 2, 3], [8, 1, 2], attr(2, 2, 'hello')]));
        const element = root.firstChild, child = element.firstChild;
        expect(element.title).toBe('hello');
        expect(child.nodeType).toBe(Node.TEXT_NODE);
        expect(element.querySelector('b')).toBeNull();
        dom.apply(packet([text(4, 'before'), [9, 2, 4, 3], [7, 3, ...str('after')], [5, 2, 2]]));
        expect(element.textContent).toBe('beforeafter');
        expect(element.lastChild).toBe(child);
        expect(element.hasAttribute('title')).toBeFalse();
        dom.apply(packet([[10, 4], [10, 2]]));
        expect(root.childNodes.length).toBe(0);
        expect(() => dom.apply(packet([[7, 3, ...str('gone')]]))).toThrow();
    });

    for (const [label, definition, operation] of [
        ['script', 'script', [2, 8, 3]],
        ['inline handler', 'onclick', attr(2, 3, 'alert(1)')],
        ['HTML property', 'innerHTML', prop(2, 3, '<img src=x onerror=alert(1)>')],
        ['unknown event', 'mouseover', [11, 2, 3, 0]],
        ['unknown property', 'outerHTML', prop(2, 3, 'bad')],
        ['masked JavaScript URL', 'href', attr(2, 3, 'java\tscript:alert(1)')],
        ['control-prefixed JavaScript URL', 'href', attr(2, 3, '\x01javascript:alert(1)')],
        ['mixed-case JavaScript URL', 'href', attr(2, 3, '  JavaScript:alert(1)')],
        ['data URL', 'src', attr(2, 3, 'data:text/html,hello')],
    ]) {
        it(`rejects the entire hostile batch: ${label}`, () => {
            expect(() => dom.apply(packet([name(3, definition), [2, 2, 1], [8, 1, 2], operation]))).toThrow();
            expect(root.childNodes.length).toBe(0);
            // A valid definition preceding the rejection is retained, including definitions after an unsafe operation.
            dom.apply(packet([[2, 2, 1], [8, 1, 2], name(4, 'id'), attr(2, 4, 'retained')]));
            expect(root.firstChild.id).toBe('retained');
        });
    }

    for (const [label, operations] of [
        ['removed node reused', [[10, 2], [8, 1, 2]]],
        ['removed descendant reused', [[10, 2], [7, 3, ...str('gone')]]],
        ['cycle', [[2, 4, 1], [8, 2, 4], [8, 4, 2]]],
        ['self-cycle', [[8, 2, 2]]],
        ['reference owned by another parent', [[2, 4, 1], [9, 4, 2, 3]]],
        ['island moved', [[8, 2, 1]]],
        ['island removed', [[10, 1]]],
        ['island modified', [attr(1, 2, 'bad')]],
        ['text replaces an element subtree', [[7, 2, ...str('bad')]]],
        ['text parent', [[8, 3, 2]]],
        ['text attribute', [attr(3, 2, 'bad')]],
        ['duplicate node', [[2, 2, 1]]],
        ['missing island', [name(3, 'missing'), [13, 5, 3]]],
        ['duplicate island binding', [[13, 5, 0]]],
    ]) {
        it(`validates the evolving tree: ${label}`, () => {
            dom.apply(packet([[2, 2, 1], text(3, 'unchanged'), [8, 2, 3], [8, 1, 2]]));
            const element = root.firstChild;
            expect(() => dom.apply(packet([[7, 3, ...str('partial')], ...operations]))).toThrow();
            expect(root.firstChild).toBe(element);
            expect(root.textContent).toBe('unchanged');
        });
    }

    it('rejects unknown, forward, oversized and non-sequential name definitions', () => {
        expect(() => dom.apply(packet([[2, 2, 9]]))).toThrow();
        expect(() => dom.apply(packet([[2, 2, 3], name(3, 'div')]))).toThrow();
        expect(() => dom.apply(packet([name(8, 'div')]))).toThrow();
        expect(() => dom.apply(packet([name(4, 'x'.repeat(1025))]))).toThrow();
    });

    it('rejects malformed headers, opcodes, UTF-8, flags and noncanonical uint32 varints', () => {
        for (const bytes of [
            new Uint8Array(), new Uint8Array([5, 0, 0, 0, 0, 0]),
            packet([[0]]), packet([[14]]), packet([[10, 0x80]]), packet([[10, 0x80, 0]]),
            packet([[10, 255, 255, 255, 255, 16]]), packet([[10, 128, 128, 128, 128, 128, 1]]),
            packet([[3, 2, 5, 65]]), packet([[3, 2, 2, 0xc0, 0x80]]),
            packet([[3, 2, 3, 0xed, 0xa0, 0x80]]), packet([[3, 2, 4, 0xf4, 0x90, 0x80, 0x80]]),
            packet([name(3, 'click'), [2, 2, 1], [11, 2, 3, 4]]),
        ]) expect(() => dom.apply(bytes)).withContext(String(bytes)).toThrow();
        expect(root.childNodes.length).toBe(0);
    });

    it('enforces exactly 1 MiB before decoding and accepts a boundary-size command', () => {
        const prefix = packet([text(2, '')]); // Build the large payload without argument/spread limits.
        const size = 1024 * 1024, length = size - 11;
        const bytes = new Uint8Array(size); bytes.fill(120);
        bytes.set(prefix.subarray(0, 8)); bytes.set(uint(length), 8);
        expect(() => dom.apply(bytes)).not.toThrow();
        expect(() => dom.apply(new Uint8Array(size + 1))).toThrow();
    });

    it('permits safe URLs and safe allowlist extensions while preserving permanent exclusions', () => {
        for (const extensions of [{tags: ['script']}, {tags: ['iframe']}, {attributes: ['onclick']},
            {attributes: ['onload']}, {attributes: ['srcdoc']}, {tags: ['SCRIPT']}])
            expect(() => install({}, extensions)).toThrow();
        const extra = install({}, {tags: ['article'], attributes: ['data-extra'], events: ['custom']});
        const other = document.createElement('div'), revoke = extra.island('extra', other);
        try {
            extra.apply(packet([name(0, 'extra'), name(1, 'article'), name(2, 'href'), name(3, 'data-extra'),
                [13, 1, 0], [2, 2, 1], attr(2, 2, '/safe'), attr(2, 3, 'yes'), [8, 1, 2]]));
            expect(other.firstChild.localName).toBe('article');
            expect(other.firstChild.dataset.extra).toBe('yes');
            for (const value of ['javascript:bad', 'java\nscript:bad', 'data:text/html,bad'])
                expect(() => extra.apply(packet([attr(2, 2, value)]))).toThrow();
        } finally { revoke(); }
    });

    it('supports typed properties, event flags, unlisten and listener cleanup on removal', async () => {
        dom.apply(packet([name(3, 'input'), name(4, 'checked'), name(5, 'click'),
            [2, 2, 3], prop(2, 4, 'true'), [8, 1, 2], [11, 2, 5, 3]]));
        const input = root.firstChild;
        expect(input.checked).toBeTrue();
        const event = new Event('click', {cancelable: true, bubbles: true});
        input.dispatchEvent(event);
        expect(event.defaultPrevented).toBeTrue();
        expect(bridge.cppFunction).toHaveBeenCalledWith('wfDom.event');
        bridge.cppFunction.calls.reset();
        dom.apply(packet([[12, 2, 5]])); input.click();
        expect(bridge.cppFunction).not.toHaveBeenCalled();
        dom.apply(packet([[11, 2, 5, 0], [10, 2]])); input.click();
        expect(bridge.cppFunction).not.toHaveBeenCalled();
    });

    it('invalidates released island handles and refuses overlapping or nonempty islands', () => {
        dom.apply(packet([[2, 2, 1], [8, 1, 2]]));
        expect(() => dom.island('overlap', root.firstChild)).toThrow();
        for (const tag of ['script', 'style', 'iframe', 'object'])
            expect(() => dom.island('unsafe', document.createElement(tag))).toThrow();
        const populated = document.createElement('div'); populated.append('page-owned');
        expect(() => dom.island('populated', populated)).toThrow();
        const again = dom.island('root', root);
        release(); // A stale release cannot revoke a newer registration.
        dom.apply(packet([text(3, 'still here'), [8, 2, 3]]));
        again();
        expect(() => dom.apply(packet([[10, 2]]))).toThrow();
        expect(root.isConnected).toBeTrue();
    });

    it('does not scan unrelated nodes or siblings for a small text or append batch', () => {
        const ops = [];
        for (let id = 2; id < 2002; ++id) ops.push(text(id, 'x'), [8, 1, ...uint(id)]);
        dom.apply(packet(ops));
        const update = packet([[7, 2, ...str('changed')], text(2002, 'new'), [8, 1, ...uint(2002)]]);
        let visited = 0;
        const originals = [Map.prototype[Symbol.iterator], Set.prototype[Symbol.iterator]];
        try {
            for (const [prototype, original] of [[Map.prototype, originals[0]], [Set.prototype, originals[1]]]) {
                prototype[Symbol.iterator] = function* () {
                    for (const value of original.call(this)) { ++visited; yield value; }
                };
            }
            dom.apply(update);
        } finally {
            Map.prototype[Symbol.iterator] = originals[0];
            Set.prototype[Symbol.iterator] = originals[1];
        }
        expect(root.firstChild.data).toBe('changed');
        expect(root.lastChild.data).toBe('new');
        expect(visited).toBeLessThan(20); // Work must not grow with the 2,000 untouched children.
    });

    it('discards rejected structural deltas and supports removing then recreating an identifier', () => {
        dom.apply(packet([[2, 2, 1], text(3, 'old'), [8, 2, 3], [8, 1, 2]]));
        expect(() => dom.apply(packet([[8, 1, 3], [8, 3, 2]]))).toThrow();
        dom.apply(packet([[10, 2], text(2, 'replacement'), [8, 1, 2]]));
        expect(root.textContent).toBe('replacement');
        expect(() => dom.apply(packet([[7, 3, ...str('gone')]]))).toThrow();
    });

    it('bounds retained name bytes across accepted and rejected batches', () => {
        const word = 'a'.repeat(1024);
        let index = 3;
        for (const count of [512, 511]) {
            const ops = [];
            for (let i = 0; i < count; ++i) ops.push(name(index++, word));
            // Definitions survive semantic rejection, but still consume the same global byte budget.
            expect(() => dom.apply(packet([...ops, [10, 99]]))).toThrow();
        }
        // root/div/title occupy 12 bytes. Fill exactly the remaining dictionary budget.
        dom.apply(packet([name(index++, 'b'.repeat(1012))]));
        expect(() => dom.apply(packet([name(index, 'x')]))).toThrow();
        dom.apply(packet([[2, 2, 1], [8, 1, 2]])); // Already-interned names remain usable.
        expect(root.firstChild.localName).toBe('div');
    });

    it('reports residual DOM exceptions as rejection instead of losing them', () => {
        dom.apply(packet([[2, 2, 1], [8, 1, 2]]));
        spyOn(root.firstChild, 'setAttribute').and.throwError('DOM failure');
        expect(() => dom.apply(packet([attr(2, 2, 'value')]))).toThrowError('DOM failure');
        expect(() => dom.apply(packet([[10, 2]]))).toThrow();
        release(); // Already invalidated: this does not erase the partial DOM left in the page.
        expect(root.childNodes.length).toBe(1);
        expect(() => dom.island('root', root)).toThrow();
        root.replaceChildren();
        release = dom.island('root', root);
        dom.apply(packet([[13, 1, 0], text(2, 'recovered'), [8, 1, 2]]));
        expect(root.textContent).toBe('recovered');
    });
});

describe('DOM commands over the real WebSocket', () => {
    it('delivers event values and listener removal through the real WebSocket', async () => {
        await webFront.ready;
        const {installDomInterpreter} = await import(moduleUrl);
        const dom = installDomInterpreter(webFront);
        const root = document.body.appendChild(document.createElement('div'));
        const release = dom.island('root', root);
        try {
            await webFront.cppFunction('sendDomBatch')(packet([
                name(0, 'root'), name(1, 'input'), name(2, 'type'), name(3, 'checked'),
                name(4, 'input'), name(5, 'change'), name(6, 'keydown'), name(7, 'click'),
                [13, 1, 0], [2, 10, 1], [2, 11, 1], attr(11, 2, 'checkbox'),
                [8, 1, 10], [8, 1, 11], [11, 10, 4, 0], [11, 11, 5, 0],
                [11, 10, 6, 0], [11, 10, 7, 3],
            ]));
            const input = root.firstChild, checkbox = root.lastChild;
            input.value = 'hello'; input.dispatchEvent(new Event('input'));
            checkbox.checked = true; checkbox.dispatchEvent(new Event('change'));
            checkbox.checked = false; checkbox.dispatchEvent(new Event('change'));
            input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}));
            const click = new Event('click', {cancelable: true}); input.dispatchEvent(click);
            expect(click.defaultPrevented).toBeTrue();
            const expected = '10:4:hello|11:5:true|11:5:false|10:6:Escape|10:7:|';
            expect(await webFront.cppFunction('domEventLog')()).toBe(expected);
            await webFront.cppFunction('sendDomBatch')(packet([[12, 10, 4], [10, 11]]));
            input.dispatchEvent(new Event('input')); checkbox.dispatchEvent(new Event('change'));
            expect(await webFront.cppFunction('domEventLog')()).toBe(expected);
        } finally { release(); root.remove(); delete webFront.domBatchHandler; }
    });

    it('round trips C++ encoding and reports correlated hostile and unloaded-interpreter rejections', async () => {
        await webFront.ready;
        const id = 0x12345678;
        // No module is installed on this bridge yet; the bridge still reports rejection to C++.
        await webFront.cppFunction('sendDomBatch')(packet([], id));
        expect(await webFront.cppFunction('domBatchRejection')(id)).toContain('not installed');
        const {installDomInterpreter} = await import(moduleUrl);
        const dom = installDomInterpreter(webFront);
        const root = document.body.appendChild(document.createElement('div'));
        const release = dom.island('root', root);
        try {
            await webFront.cppFunction('sendDomBatch')(packet([name(0, 'root'), name(1, 'div'), name(2, 'href'),
                [13, 1, 0], [2, 2, 1], text(0xffffffff, 'C++ → été 🌍'), [8, 2, ...uint(0xffffffff)], [8, 1, 2]], id + 1));
            expect(root.textContent).toBe('C++ → été 🌍');
            await webFront.cppFunction('sendDomBatch')(packet([text(4, 'partial'), [8, 1, 4], attr(2, 2, 'java\tscript:bad')], id + 2));
            expect(root.textContent).toBe('C++ → été 🌍');
            expect(await webFront.cppFunction('domBatchRejection')(id + 2)).toContain('unsafe DOM URL');
            spyOn(root.firstChild, 'setAttribute').and.throwError('x'.repeat(70000));
            await webFront.cppFunction('sendDomBatch')(packet([attr(2, 2, '/safe')], id + 3));
            expect((await webFront.cppFunction('domBatchRejection')(id + 3)).length).toBe(4096);
            // Truncated and oversized commands are rejected by the dispatch path as well.
            webFront.onMessage(new Uint8Array([5]).buffer);
            expect(await webFront.cppFunction('domBatchRejection')(0)).not.toBe('');
            webFront.onMessage(new Uint8Array(1024 * 1024 + 1).fill(5).buffer);
            expect(await webFront.cppFunction('domBatchRejection')(0x05050505)).toContain('1 MiB');
        } finally { release(); root.remove(); delete webFront.domBatchHandler; }
    });
});
