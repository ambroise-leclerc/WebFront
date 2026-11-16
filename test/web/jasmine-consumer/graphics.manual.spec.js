// Manual GPU qualification, deliberately outside CTest's software-rendered suite.
// See docs/JETSON.md. Run on the real desktop without renderer overrides first.
describe('Native graphics qualification', () => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 256;
    const gl = canvas.getContext('webgl', {preserveDrawingBuffer: true});
    const debug = gl && gl.getExtension('WEBGL_debug_renderer_info');
    const renderer = gl && gl.getParameter(debug ? debug.UNMASKED_RENDERER_WEBGL : gl.RENDERER);
    const label = document.createElement('p');
    label.textContent = `WebGL renderer: ${renderer || 'context unavailable'}`;
    document.body.append(label);

    it('renders Canvas 2D pixels', () => {
        const flat = document.createElement('canvas');
        flat.width = flat.height = 256;
        const context = flat.getContext('2d');
        expect(context).not.toBeNull();
        context.fillStyle = '#ff0000';
        context.fillRect(0, 0, 256, 256);
        expect(Array.from(context.getImageData(128, 128, 1, 1).data)).toEqual([255, 0, 0, 255]);
        document.body.append(flat);
    });

    it(`renders WebGL pixels (${renderer || 'context unavailable'})`, () => {
        expect(gl).not.toBeNull();
        if (!gl) return;
        gl.clearColor(0, 1, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        const pixel = new Uint8Array(4);
        gl.readPixels(128, 128, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
        expect(gl.getError()).toBe(gl.NO_ERROR);
        expect(Array.from(pixel)).toEqual([0, 255, 0, 255]);
        document.body.append(canvas);
    });

    it('captures the composed page for visual inspection', async () => {
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        expect(await webFrontCapture.screenshot('graphics')).toBeGreaterThan(100);
    });
});
