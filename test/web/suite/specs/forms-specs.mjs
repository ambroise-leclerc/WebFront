// The forms of #383 in a real browser: the events of real controls write the C++ object, a refused value marks the input,
// and C++ changes come back through the controls. The same page and interpreter as dom-document-specs.mjs.
async function until(predicate) {
    const deadline = performance.now() + 5000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error('forms condition timed out');
        await new Promise(resolve => setTimeout(resolve, 5));
    }
}
async function connectedFrame() {
    const frame = document.createElement('iframe');
    const loaded = new Promise((resolve, reject) => { frame.onload = resolve; frame.onerror = reject; });
    frame.src = '/dom-document.html';
    document.body.appendChild(frame);
    await loaded;
    await until(() => !!frame.contentWindow.documentConnected);
    await frame.contentWindow.documentConnected;
    return frame;
}
const call = (frame, name, ...args) => frame.contentWindow.webFront.cppFunction(name)(...args);
const state = frame => call(frame, 'formsState');
const stateIs = (frame, expected) => async () => await state(frame) === expected;

describe('C++ forms', () => {
    it('writes the object from real controls, marks refused values and shows C++ changes', async () => {
        const frame = await connectedFrame();
        const page = frame.contentWindow;
        try {
            await call(frame, 'formsBuild');
            const root = page.document.getElementById('root');
            await until(() => root.querySelector('form') && root.querySelector('table'));
            const form = root.querySelector('form');
            const {speed, density, wrap, pattern, title} = form.elements;
            const fire = (control, type) => control.dispatchEvent(new page.Event(type, {bubbles: true, cancelable: true}));
            const initial = 'speed=5;density=0.25;wrap=true;pattern=glider;title=Life';

            expect(speed.type).toBe('number');
            expect([speed.min, speed.max, speed.step]).toEqual(['1', '10', '1']);
            expect(speed.value).toBe('5');
            expect(wrap.checked).toBeTrue();
            expect(pattern.selectedIndex).toBe(0);
            expect(await state(frame)).toBe(initial);

            // Valid values reach C++ in the form the description parses.
            speed.value = '7'; fire(speed, 'input');
            await until(stateIs(frame, 'speed=7;density=0.25;wrap=true;pattern=glider;title=Life'));
            density.value = '0.5'; fire(density, 'input');
            wrap.checked = false; fire(wrap, 'change');
            pattern.selectedIndex = 2; fire(pattern, 'change');
            title.value = 'Café 🌍'; fire(title, 'input');
            await until(stateIs(frame, 'speed=7;density=0.5;wrap=false;pattern=random;title=Café 🌍'));
            expect(speed.hasAttribute('aria-invalid')).toBeFalse();

            // A value outside the bounds, and the empty text of an invalid number, are refused: the member keeps its value.
            speed.value = '99'; fire(speed, 'input');
            await until(() => speed.getAttribute('aria-invalid') === 'true');
            expect((await state(frame)).startsWith('speed=7;')).toBeTrue();
            speed.value = ''; fire(speed, 'input');
            await new Promise(resolve => setTimeout(resolve, 50));
            expect(speed.getAttribute('aria-invalid')).toBe('true');
            expect((await state(frame)).startsWith('speed=7;')).toBeTrue();
            speed.value = '3'; fire(speed, 'input');
            await until(() => !speed.hasAttribute('aria-invalid'));
            expect((await state(frame)).startsWith('speed=3;')).toBeTrue();

            // Enter in a text input submits the form, and the page must not navigate.
            const submit = new page.Event('submit', {bubbles: true, cancelable: true});
            form.dispatchEvent(submit);
            expect(submit.defaultPrevented).toBeTrue();

            // C++ changes the object and refreshes: the controls and the inspector follow, and the invalid mark goes.
            speed.value = '99'; fire(speed, 'input');
            await until(() => speed.getAttribute('aria-invalid') === 'true');
            await call(frame, 'formsChange');
            await until(() => speed.value === '9' && !speed.hasAttribute('aria-invalid'));
            await until(() => density.value === '0.5' && wrap.checked && pattern.selectedIndex === 1 && title.value === 'From C++');
            await until(() => [...root.querySelectorAll('td')].map(cell => cell.textContent).join('|') === '9|0.5|true|pulsar|From C++');
            expect(await state(frame)).toBe('speed=9;density=0.5;wrap=true;pattern=pulsar;title=From C++');
        } finally { frame.remove(); }
    });
});
