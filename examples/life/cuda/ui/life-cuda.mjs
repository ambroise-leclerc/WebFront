/// Page of the Life CUDA demo. C++ owns the field and computes the generations; this module asks for the next frame
/// when it has drawn the previous one (so one frame at most is in flight), uploads the packed cells as they are
/// into a texture and lets the fragment shader read the bits. `webFront` comes from the classic script WebFront.js.

const element = id => document.getElementById(id);
const canvas = element('field');
const call = (name, ...args) => webFront.cppFunction(name)(...args);

let side = 0;
let cells = new Uint8Array(0);
// Field coordinates of the canvas' top-left corner, and cells per device pixel.
const view = {x: 0, y: 0, scale: 1};

// --- Rendering ------------------------------------------------------------------------------------------------

const vertexShader = `#version 300 es
void main() {
    // One triangle covering the viewport.
    gl_Position = vec4(float((gl_VertexID << 1) & 2) * 2. - 1., float(gl_VertexID & 2) * 2. - 1., 0., 1.);
}`;
const fragmentShader = `#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D cells;   // one byte per texel: eight cells, least significant bit first
uniform vec2 origin;
uniform float scale;
uniform float side;
uniform float height;
out vec4 color;
float alive(vec2 position) {
    ivec2 cell = ivec2(mod(floor(position), side));
    uint packed = texelFetch(cells, ivec2(cell.x >> 3, cell.y), 0).r;
    return float((packed >> uint(cell.x & 7)) & 1u);
}
void main() {
    vec2 pixel = vec2(gl_FragCoord.x, height - gl_FragCoord.y);
    vec2 position = origin + pixel * scale;
    float density = alive(position);
    if (scale > 1.) {
        // Several cells per pixel: sample a 4 x 4 grid of them so that sparse patterns do not flicker away.
        density = 0.;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                density += alive(origin + (pixel - .5 + (vec2(i, j) + .5) / 4.) * scale);
        density = min(1., density / 6.);
    }
    color = vec4(mix(vec3(.024, .078, .059), vec3(.506, .937, .741), density), 1.);
}`;

function createWebGLRenderer() {
    const gl = canvas.getContext('webgl2', {antialias: false, alpha: false});
    if (!gl) return null;
    const program = gl.createProgram();
    for (const [type, source] of [[gl.VERTEX_SHADER, vertexShader], [gl.FRAGMENT_SHADER, fragmentShader]]) {
        const shader = gl.createShader(type);
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
        gl.attachShader(program, shader);
    }
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
    gl.useProgram(program);
    gl.bindVertexArray(gl.createVertexArray());
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const uniform = name => gl.getUniformLocation(program, name);
    const [origin, scale, sideUniform, height] = ['origin', 'scale', 'side', 'height'].map(uniform);
    let uploaded = null;
    return {
        name: 'WebGL 2',
        draw() {
            if (uploaded !== cells) {
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, side / 8, side, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, cells);
                uploaded = cells;
            }
            gl.viewport(0, 0, canvas.width, canvas.height);
            gl.uniform2f(origin, view.x, view.y);
            gl.uniform1f(scale, view.scale);
            gl.uniform1f(sideUniform, side);
            gl.uniform1f(height, canvas.height);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
        },
    };
}

// Without WebGL 2 the bits are unpacked in JavaScript: slower, and the reason the shader path exists.
function createCanvasRenderer() {
    const context = canvas.getContext('2d');
    const buffer = document.createElement('canvas');
    let image = null, pixels = null, unpacked = null;
    return {
        name: 'Canvas 2D',
        draw() {
            if (unpacked !== cells) {
                if (buffer.width !== side) {
                    buffer.width = buffer.height = side;
                    image = buffer.getContext('2d').createImageData(side, side);
                    pixels = new Uint32Array(image.data.buffer);
                }
                for (let i = 0; i < pixels.length; ++i) pixels[i] = cells[i >> 3] & (1 << (i & 7)) ? 0xffbdef81 : 0xff0f1406;
                buffer.getContext('2d').putImageData(image, 0, 0);
                unpacked = cells;
            }
            context.imageSmoothingEnabled = view.scale > 1;
            const size = side / view.scale;
            const wrap = value => ((value % side) + side) % side;
            for (let y = -wrap(view.y) / view.scale; y < canvas.height; y += size)
                for (let x = -wrap(view.x) / view.scale; x < canvas.width; x += size) context.drawImage(buffer, x, y, size, size);
        },
    };
}

// Start-up options from the address: index.html?backend=cpu&generations=16&renderer=canvas&pixelratio=1
const options = new URLSearchParams(location.search);
// Device pixels per CSS pixel of the canvas: lowering it is what helps most where WebGL is rendered in software.
const maxPixelRatio = Number(options.get('pixelratio')) || Infinity;
let renderer = null;
let drawRequested = false;
function draw() {
    if (drawRequested || !renderer || !side) return;
    drawRequested = true;
    requestAnimationFrame(() => { drawRequested = false; renderer.draw(); });
}

function resetView() {
    const fit = side / Math.min(canvas.width, canvas.height);
    view.scale = fit;
    view.x = (side - canvas.width * fit) / 2;
    view.y = (side - canvas.height * fit) / 2;
}
function resize() {
    const ratio = Math.min(window.devicePixelRatio || 1, maxPixelRatio);
    const width = Math.round(canvas.clientWidth * ratio), height = Math.round(canvas.clientHeight * ratio);
    if (canvas.width === width && canvas.height === height) return;
    // Keep the cell under the centre of the canvas where it is.
    view.x += (canvas.width - width) * view.scale / 2;
    view.y += (canvas.height - height) * view.scale / 2;
    canvas.width = width; canvas.height = height;
    draw();
}

// --- Pan and zoom ---------------------------------------------------------------------------------------------

function devicePoint(event) {
    const rect = canvas.getBoundingClientRect();
    return {x: (event.clientX - rect.left) * canvas.width / rect.width, y: (event.clientY - rect.top) * canvas.height / rect.height};
}
canvas.addEventListener('wheel', event => {
    event.preventDefault();
    const point = devicePoint(event);
    // From 64 device pixels per cell to the whole field on 128 pixels.
    const scale = Math.min(Math.max(view.scale * Math.exp(event.deltaY * .0015), 1 / 64), side / 128);
    view.x += point.x * (view.scale - scale);
    view.y += point.y * (view.scale - scale);
    view.scale = scale;
    draw();
}, {passive: false});
let drag = null;
canvas.addEventListener('pointerdown', event => {
    drag = devicePoint(event);
    canvas.setPointerCapture(event.pointerId);
    canvas.classList.add('dragging');
});
canvas.addEventListener('pointermove', event => {
    if (!drag) return;
    const point = devicePoint(event);
    view.x -= (point.x - drag.x) * view.scale;
    view.y -= (point.y - drag.y) * view.scale;
    drag = point;
    draw();
});
for (const name of ['pointerup', 'pointercancel']) canvas.addEventListener(name, () => { drag = null; canvas.classList.remove('dragging'); });
canvas.addEventListener('dblclick', () => { resetView(); draw(); });

// --- Simulation -----------------------------------------------------------------------------------------------

const state = {playing: false, pending: false, backend: 0, generationsPerFrame: 1};
// Smoothed over about a second of frames, so the figures can be read.
const measure = {rate: 0, compute: 0, roundTrip: 0, frames: 0, last: 0, shown: 0};
const smooth = (previous, value) => previous ? previous + (value - previous) * .1 : value;
const format = (value, digits = 0) => value.toLocaleString('en-US', {maximumFractionDigits: digits, minimumFractionDigits: digits});

function showError(error) {
    element('error').textContent = error ? String(error.message ?? error) : '';
}
function setPlaying(playing) {
    state.playing = playing;
    element('play').textContent = playing ? 'Pause' : 'Play';
    element('play').setAttribute('aria-pressed', String(playing));
    element('step').disabled = playing;
    if (playing) { measure.last = 0; void run(); }
}
function showFrame([packed, generation, population]) {
    if (!(packed instanceof Uint8Array) || packed.length !== side * side / 8) throw new Error('Invalid frame from C++');
    cells = packed;
    element('generation').textContent = format(generation);
    element('population').textContent = format(population);
    draw();
}
function showMeasurements(now) {
    if (now - measure.shown < 250) return;
    measure.shown = now;
    element('rate').textContent = format(measure.rate);
    element('compute').textContent = format(measure.compute, 1) + ' µs';
    element('round-trip').textContent = format(measure.roundTrip, 1) + ' ms';
    element('frames').textContent = format(measure.frames, 1);
}

// One call at a time: a frame is asked for only when the previous one has been received.
async function request(name, ...args) {
    if (state.pending) return false;
    state.pending = true;
    try {
        const generations = name === 'life.cuda.advance' ? args[1] : 0;
        const sent = performance.now();
        const frame = await call(name, ...args);
        const received = performance.now();
        showFrame(frame);
        showError('');
        if (generations) {
            measure.compute = smooth(measure.compute, frame[3] / generations);
            measure.roundTrip = smooth(measure.roundTrip, received - sent);
            if (measure.last) {
                measure.rate = smooth(measure.rate, generations * 1000 / (received - measure.last));
                measure.frames = smooth(measure.frames, 1000 / (received - measure.last));
            }
            measure.last = received;
            showMeasurements(received);
        }
        return true;
    } catch (error) {
        showError(error);
        setPlaying(false);
        return false;
    } finally {
        state.pending = false;
    }
}
async function run() {
    while (state.playing) {
        await request('life.cuda.advance', state.backend, state.generationsPerFrame);
        // Yield to the browser so the frame is painted and input handled before the next one is asked for.
        await new Promise(resolve => requestAnimationFrame(resolve));
    }
}
// A user action waits for the frame in flight rather than being dropped.
async function act(name, ...args) {
    while (!await request(name, ...args)) {
        if (element('error').textContent) return;
        await new Promise(resolve => setTimeout(resolve, 4));
    }
}

element('play').addEventListener('click', () => setPlaying(!state.playing));
element('step').addEventListener('click', () => void act('life.cuda.advance', state.backend, state.generationsPerFrame));
for (const button of document.querySelectorAll('[data-pattern]'))
    button.addEventListener('click', () => void act('life.cuda.seed', Number(button.dataset.pattern), Date.now() >>> 0));
for (const input of document.querySelectorAll('input[name="backend"]'))
    input.addEventListener('change', () => { state.backend = Number(input.value); Object.assign(measure, {rate: 0, compute: 0, roundTrip: 0, frames: 0, last: 0}); });
element('load').addEventListener('input', event => {
    state.generationsPerFrame = 2 ** Number(event.target.value);
    element('load-value').textContent = String(state.generationsPerFrame);
    measure.last = 0;
});
const toggleFullscreen = () => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen();
element('fullscreen').hidden = !document.fullscreenEnabled;
element('fullscreen').addEventListener('click', () => void toggleFullscreen().catch(showError));
document.addEventListener('keydown', event => {
    if (event.key === 'f' && document.fullscreenEnabled) void toggleFullscreen().catch(showError);
    if (event.key === ' ' && event.target === document.body) { event.preventDefault(); setPlaying(!state.playing); }
});

try {
    await webFront.ready;
    const [fieldSide, cudaAvailable, device] = await call('life.cuda.info');
    side = fieldSide;
    element('size').textContent = `${side} × ${side}`;
    element('message').textContent = `${format(side * side / 8 / 1024)} KiB`;
    element('device').textContent = cudaAvailable ? device : 'CUDA is not available in this build or on this machine';
    const cuda = document.querySelector('input[name="backend"][value="1"]');
    cuda.disabled = !cudaAvailable;
    // CUDA when there is one, unless the address says otherwise.
    if (cudaAvailable && options.get('backend') !== 'cpu') { cuda.checked = true; state.backend = 1; }
    const exponent = Math.round(Math.log2(Number(options.get('generations')) || 1));
    element('load').value = String(Math.min(Math.max(exponent, 0), 10));
    element('load').dispatchEvent(new Event('input'));
    renderer = (options.get('renderer') === 'canvas' ? null : createWebGLRenderer()) ?? createCanvasRenderer();
    element('renderer').textContent = renderer.name;
    new ResizeObserver(resize).observe(canvas);
    resize();
    resetView();
    await act('life.cuda.advance', 0, 0);
    setPlaying(true);
} catch (error) {
    showError(error);
    element('device').textContent = 'Not connected';
}
