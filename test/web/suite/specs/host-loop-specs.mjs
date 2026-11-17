// The host-loop example (examples/host-loop) in a real browser: C++ adds the example's page to this document as the
// example does, and a loop on a thread of the test program computes the images. These specs check what the page shows,
// that its controls reach the loop, the contract of the bridge and the stop. C++ checks that its loop ended on request.
const call = (name, ...args) => webFront.cppFunction(name)(...args);
const until = async (predicate, what) => {
    const deadline = performance.now() + 15000;
    for (;;) {
        const value = await predicate();
        if (value) return value;
        if (performance.now() > deadline)
            throw new Error(`Host loop: timed out waiting for ${what}; the page says "${document.querySelector('#host-loop .status')?.textContent}"`);
        await new Promise(resolve => setTimeout(resolve, 10));
    }
};
// The pixels of an image as it crossed the bridge: as they are, or expanded from their runs.
const pixelsOf = ([data, [, width, height, , , packed]]) => {
    if (!packed) return data;
    const pixels = new Uint32Array(width * height);
    for (let i = 0, at = 0; i < data.length; at += data[i], i += 2) pixels.fill(data[i + 1], at, at + data[i]);
    return pixels;
};
// Pixels of two images whose red, green or blue differ by more than two levels.
const differing = (first, second) => {
    const x = new Uint8Array(pixelsOf(first).buffer), y = new Uint8Array(pixelsOf(second).buffer);
    let count = 0;
    for (let i = 0; i < x.length; i += 4)
        if (Math.abs(x[i] - y[i]) > 2 || Math.abs(x[i + 1] - y[i + 1]) > 2 || Math.abs(x[i + 2] - y[i + 2]) > 2) ++count;
    return count;
};

describe('Host-loop example', () => {
    const limit = 40000;  // above the waits of a spec: a loaded machine fails with what was awaited, not with a bare timeout
    // [data, [sequence, width, height, rays, revision, packed], [engine, paused, glass, quality], [computeMs, copyMs, packMs]]
    const frames = [];
    let root, canvas, stats, status, requests = 0, device;
    const control = name => root.querySelector(`[name=${name}]`);
    const change = (name, property, value) => {
        control(name)[property] = value;
        control(name).dispatchEvent(new Event('change'));
    };
    // The first image received from now on that satisfies `predicate`.
    const nextFrame = (predicate, what) => {
        const from = frames.length;
        return until(() => frames.slice(from).find(predicate), what);
    };
    const settings = (engine, paused, glass, quality) => ([, , used]) =>
        used[0] === engine && used[1] === paused && used[2] === glass && used[3] === quality;

    beforeAll(async () => {
        await webFront.ready;
        [device] = await call('host.info');
        // Count the page's requests before it makes the first one.
        const cppFunction = webFront.cppFunction.bind(webFront);
        spyOn(webFront, 'cppFunction').and.callFake(name => {
            if (name === 'host.nextFrame') ++requests;
            return cppFunction(name);
        });
        Object.defineProperty(globalThis, 'hostLoop', {
            configurable: true,
            set(page) {
                const onFrame = page.onFrame;
                page.onFrame = (...frame) => { frames.push(frame); onFrame(...frame); };
                Object.defineProperty(globalThis, 'hostLoop', {configurable: true, writable: true, value: page});
            }
        });
        await call('hostLoopTest.mount');
        root = await until(() => document.querySelector('#host-loop'), 'the page');
        canvas = root.querySelector('canvas');
        stats = root.querySelector('.stats');
        status = root.querySelector('.status');
        expect(status.getAttribute('role')).toBe('status');
        expect(status.textContent).toBe('');
        expect(getComputedStyle(status).display).not.toBe('none');
        expect(getComputedStyle(status).visibility).toBe('visible');
        await nextFrame(() => true, 'the first image');
    }, limit);

    // The stop comes last whatever the order of the specs: afterwards the host answers nothing.
    afterAll(async () => {
        try {
            expect(await call('hostLoopTest.ended')).toBeFalse();
            control('stop').click();
            expect(status.textContent).toContain('Stop requested');
            await until(() => call('hostLoopTest.ended'), 'the end of the host loop');
            const received = frames.length;
            await call('host.nextFrame', 0, 0, 0);
            await new Promise(resolve => setTimeout(resolve, 300));
            expect(frames.length).toBe(received);
        } finally {
            root?.remove();
            delete globalThis.hostLoop;
        }
    }, limit);

    it('draws the image computed by the host on its canvas', async () => {
        change('quality', 'value', '0');
        change('glass', 'checked', false);
        change('paused', 'checked', false);
        await nextFrame(settings(0, false, false, 0), 'a small opaque image');
        await until(() => canvas.width === 320 && canvas.height === 240 && /image \d+: 320 x 240, 1 ray per pixel, CPU, 2 threads, sent as runs \(\d+ % of the pixels\)/.test(stats.textContent),
            'the small image on the canvas');
        const shown = canvas.getContext('2d').getImageData(0, 0, 320, 240).data;
        expect(shown[4 * 160 + 2]).toBeGreaterThan(shown[4 * 160] + 20);  // the sky, at the top
        let red = 0, opaque = 0;
        for (let i = 0; i < shown.length; i += 4) {
            if (shown[i] > 120 && shown[i + 1] < 80 && shown[i + 2] < 80) ++red;
            if (shown[i + 3] === 255) ++opaque;
        }
        expect(red).toBeGreaterThan(200);  // the red squares of the ball
        expect(opaque).toBe(320 * 240);
        expect(stats.textContent).toMatch(/compute +[\d.]+ ms +copy +0\.0 ms +pack +[\d.]+ ms +bridge +[\d.]+ ms +draw +[\d.]+ ms\ndisplayed \d+ images per second/);
    }, limit);

    it('receives the opaque image as runs and the glass one as pixels, one image per request', async () => {
        change('quality', 'value', '0');
        change('glass', 'checked', false);
        change('paused', 'checked', false);
        const [runs, [sequence, width, height, rays, revision, packed], used, times] = await nextFrame(settings(0, false, false, 0), 'an opaque image');
        expect(runs).toBeInstanceOf(Uint32Array);
        expect(packed).toBeTrue();
        expect(runs.length % 2).toBe(0);
        expect(runs.length).toBeLessThan(width * height / 2);
        let covered = 0;
        for (let i = 0; i < runs.length; i += 2) covered += runs[i];
        expect(covered).toBe(width * height);
        expect([width, height, rays]).toEqual([320, 240, 1]);
        expect(revision).toBeGreaterThan(0);
        expect(used.map(value => typeof value)).toEqual(['number', 'boolean', 'boolean', 'number']);
        expect(times.length).toBe(3);
        expect(times[0]).toBeGreaterThan(0);
        expect(times[1]).toBe(0);
        change('glass', 'checked', true);
        const [pixels, [, , , , , glassPacked]] = await nextFrame(settings(0, false, true, 0), 'a glass image');
        expect(pixels).toBeInstanceOf(Uint32Array);
        expect(glassPacked).toBeFalse();
        expect(pixels.length).toBe(width * height);
        change('glass', 'checked', false);
        const later = await nextFrame(([, [number]]) => number > sequence + 2, 'more images');
        expect(later[1][0]).toBeGreaterThan(sequence);
        // The page asks once it has drawn: never more images than requests, and at most one request unanswered.
        expect(frames.length).toBeLessThanOrEqual(requests);
        expect(requests - frames.length).toBeLessThanOrEqual(1);
        expect(frames.map(([, [number]]) => number)).toEqual(frames.map(([, [number]]) => number).sort((a, b) => a - b));
    }, limit);

    it('applies quality, glass and pause from its controls', async () => {
        change('quality', 'value', '2');
        change('paused', 'checked', true);
        change('glass', 'checked', false);
        const large = await nextFrame(settings(0, true, false, 2), 'a paused opaque image');
        expect(large[1].slice(1, 4)).toEqual([640, 480, 4]);
        await until(() => canvas.width === 640 && canvas.height === 480, 'the larger canvas');
        expect(differing(large, await nextFrame(settings(0, true, false, 2), 'a second paused image'))).toBe(0);
        // The beams of the glass ball are costly on the two CPU threads of this test: a small image is enough.
        change('quality', 'value', '0');
        const opaque = await nextFrame(settings(0, true, false, 0), 'a small paused opaque image');
        change('glass', 'checked', true);
        const glass = await nextFrame(settings(0, true, true, 0), 'a paused glass image');
        expect(differing(opaque, glass)).toBeGreaterThan(10000);  // the ball, the dimmer day and the beams
        change('paused', 'checked', false);
        const moving = await nextFrame(settings(0, false, true, 0), 'a moving image');
        await nextFrame(frame => settings(0, false, true, 0)(frame) && differing(moving, frame) > 100, 'the ball to move');
        await until(() => control('glass').checked && !control('paused').checked && control('quality').value === '0', 'the controls to follow');
    }, limit);

    it('rejects settings the host cannot honour and keeps the previous ones', async () => {
        change('quality', 'value', '0');
        change('glass', 'checked', false);
        change('paused', 'checked', false);
        await nextFrame(settings(0, false, false, 0), 'the starting settings');
        await expectAsync(call('host.settings', 0, false, false, 4)).toBeRejected();
        await expectAsync(call('host.settings', 2, false, false, 0)).toBeRejected();
        await expectAsync(call('host.settings', 0, false, false, -1)).toBeRejected();
        const [, , used] = await nextFrame(() => true, 'an image after the refusals');
        expect(used).toEqual([0, false, false, 0]);
    }, limit);

    it('computes with CUDA where a device exists, and says so where none does', async () => {
        change('quality', 'value', '1');
        change('glass', 'checked', true);
        change('paused', 'checked', true);
        const onCpu = await nextFrame(settings(0, true, true, 1), 'a paused CPU image');
        if (!device) {
            expect(control('cuda').disabled).toBeTrue();
            expect(control('cuda').textContent).toContain('unavailable');
            await expectAsync(call('host.settings', 1, true, true, 1)).toBeRejectedWithError(/CUDA is unavailable/);
        } else {
            expect(control('cuda').disabled).toBeFalse();
            expect(control('cuda').getAttribute('aria-pressed')).toBe('false');
            control('cuda').click();
            const onCuda = await nextFrame(settings(1, true, true, 1), 'a paused CUDA image');
            const [computeMs, copyMs] = onCuda[3];
            expect(differing(onCpu, onCuda)).toBeLessThanOrEqual(640 * 480 / 100);
            expect(computeMs).toBeGreaterThan(0);
            expect(copyMs).toBeGreaterThan(0);
            await until(() => /CUDA, sent as pixels\n/.test(stats.textContent), 'the engine on the page');
            // The switch is lit and names the device while CUDA computes.
            expect(control('cuda').getAttribute('aria-pressed')).toBe('true');
            expect(control('cuda').classList.contains('on')).toBeTrue();
            expect(control('cuda').textContent).toContain(`${device} · on`);
            control('cuda').click();
            await nextFrame(settings(0, true, true, 1), 'a paused CPU image again');
            await until(() => control('cuda').getAttribute('aria-pressed') === 'false' && /off · CPU, 2 threads/.test(control('cuda').textContent),
                'the switch to go dark');
        }
        change('paused', 'checked', false);
        await nextFrame(settings(0, false, true, 1), 'the CPU again');
    }, limit);
});
