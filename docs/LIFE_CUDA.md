# Game of Life on CUDA

`examples/life/cuda/` is the Game of Life of the README at 2048 × 2048 cells instead of 32 × 32: the
field stays in C++, each generation is computed on the CPU or, when the build found a CUDA toolkit, on
the GPU, and the page draws the packed cells it receives. It was written for the Jetson Orin (#530)
and builds and runs without CUDA everywhere else.

It is one offline executable: the page is plain HTML and JavaScript embedded at build time, so there is
no `wf` step, no JavaScript runtime and no download.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target WebFrontLifeCuda
build/examples/WebFrontLifeCuda              # the window
build/examples/WebFrontLifeCuda --benchmark  # generations per second, without the bridge or the page
```

Like the other Life stages it opens a native window when the platform's engine was built and the
default browser otherwise. Drag to pan, scroll to zoom, double-click to reset the view, `F` for full
screen, space to pause. The window's starting state can be chosen on the command line:

```sh
build/examples/WebFrontLifeCuda --backend cpu --generations 16 --renderer canvas --pixelratio 1
```

`--backend` is `cpu` or `cuda` (default: CUDA when available), `--generations` the generations per
frame (1 to 1024), `--renderer` `webgl` or `canvas`, and `--pixelratio` caps the canvas' device pixels
per CSS pixel. The executable passes them to the page as `index.html?backend=…`, which a browser
accepts too.

## Build with CUDA

`WEBFRONT_LIFE_CUDA` is `AUTO` by default: the CUDA unit is built when CMake finds a working CUDA
compiler, and the example falls back to the CPU kernel otherwise, without an error. `ON` requires
CUDA and `OFF` ignores it. The configuration prints which one was chosen:

```text
-- Life CUDA example: CUDA 12.6.68 backend enabled (/usr/local/cuda-12.6/bin/nvcc)
```

Two constraints of `nvcc` shape the code (`cmake/LifeCuda.cmake`):

- **`nvcc` does not compile C++23.** The kernel is one unit, `LifeField.cu`, compiled at C++17. It
  includes `LifeField.hpp` only, which includes no WebFront header and exposes plain functions.
- **`nvcc` bounds its host compiler.** CUDA 12.6 (JetPack 6) refuses GCC newer than 13. The unit may
  be compiled with another host compiler than the rest of the build: set `CMAKE_CUDA_HOST_COMPILER`
  (or `CUDAHOSTCXX`) when the default one is refused.

The CPU and the GPU run the same function, `nextWord`: 64 cells per machine word, the eight neighbours
summed by bit-sliced adders. The CPU backend calls it in a single-threaded loop, the CUDA backend from
one thread per word. A build without CUDA therefore tests the code the GPU runs.

## Bridge contract

The field belongs to C++, one per connected browser. The page calls:

| Call | Answer |
| --- | --- |
| `life.cuda.info()` | `[side, cudaAvailable, deviceName]` |
| `life.cuda.seed(pattern, seed)` | the frame of generation 0 (`pattern`: 0 soup, 1 gliders, 2 acorn, 3 empty) |
| `life.cuda.advance(backend, generations)` | the frame `generations` later (`backend`: 0 CPU, 1 CUDA; at most 1024 generations, 0 returns the current frame) |

A frame is the tuple `[cells, generation, livingCells, microseconds]`. `cells` is a `Uint8Array` of
`side * side / 8` bytes (512 KiB), cell `y * side + x` in bit `i % 8` of byte `i / 8`: the layout the
bridge gives a `std::bitset`. C++ sends it as a typed array rather than a `std::bitset<4194304>`
because the kernel works on 64-bit words and a bitset gives no access to its words: converting bit by
bit would cost more than the generation. `microseconds` is the time the generations took in C++.

- **The page pulls.** It asks for the next frame when it has drawn the previous one, so one frame at
  most is in flight and none queues when the page is slower than the computation. To compute faster
  than the page draws, raise the generations per frame, not the call rate.
- **No silent fallback.** Asking for CUDA where it is unavailable, more than 1024 generations, an
  unknown backend or an unknown pattern rejects the Promise and leaves the field unchanged.
- **The page colours.** The bytes go unchanged into a WebGL 2 texture (`R8UI`, 256 × 2048) and the
  fragment shader reads the bits; panning, zooming and colours cost no round trip. Without WebGL 2
  the page unpacks the bits into a 2D canvas.

`advance` runs on the server thread: the bound of 1024 generations keeps a CPU call near half a
second on the Orin Nano, and the page stays responsive meanwhile because the call is a Promise.

## Tests

- `test/unit/LifeCudaTests.cpp` compares the word-parallel kernel with a cell-by-cell reference on
  fields one, two and three words wide, checks the packed layout and the patterns, and, where a CUDA
  device answers, requires the CUDA backend to equal the CPU one after 1, 2 and 61 generations in one
  call, up to the demo's 2048 × 2048 field. Without CUDA it checks that asking for it fails cleanly.
- `test/web/suite/specs/life-cuda-specs.mjs` exercises the contract above in a real browser,
  including the CPU/CUDA comparison across the bridge when the build has CUDA.

## Measurements on a Jetson Orin Nano

Measured on 2026-11-12 on an Orin Nano Super 8 GB: JetPack 6 (L4T R36.4.7, Ubuntu 22.04, kernel
5.15.148-tegra), CUDA 12.6.68, power mode `MAXN_SUPER`, GCC 13.4 (`nvcc` host compiler GCC 11.4),
Release build. One board, one run each: orders of magnitude, not a benchmark suite. Other power modes
and other Orin models were not measured.

**Computation alone** (`WebFrontLifeCuda --benchmark`, generations per second):

| Generations per call | CPU, one thread | CUDA | CUDA / CPU |
| ---: | ---: | ---: | ---: |
| 1 | 1 822 | 1 313 | 0.7 × |
| 16 | 1 802 | 12 931 | 7.2 × |
| 256 | 1 825 | 39 454 | 21.6 × |

The GPU needs work to amortize its transfers: every call uploads and downloads the 512 KiB field, so
at one generation per call the CPU is faster. The copies are plain `cudaMemcpy`; the unified memory of
the Tegra, which could avoid them, is not used yet.

**In the native window** (WebKitGTK 2.50.4, X11 session, 2160 × 3840 portrait screen at scale 2, window
of about 2160 × 1600 device pixels, soup pattern, figures read from the page after ten seconds):

| Configuration | C++ per generation | Bridge round trip | Frames / s | Generations / s |
| --- | ---: | ---: | ---: | ---: |
| CUDA, 1 per frame | 1 773 µs | 172 ms | 5.4 | 5 |
| CUDA, 16 per frame | 152 µs | 172 ms | 5.5 | 87 |
| CUDA, 256 per frame | 107 µs | 172 ms | 5.4 | 1 388 |
| CPU, 1 per frame | 994 µs | 173 ms | 5.4 | 5 |
| CPU, 16 per frame | 1 101 µs | 172 ms | 5.4 | 86 |
| CUDA, 16 per frame, `--pixelratio 1` | 196 µs | 88 ms | 10.2 | 164 |
| CUDA, 16 per frame, `--pixelratio 1 --renderer canvas` | 300 µs | 48 ms | 11.3 | 181 |
| CUDA, 16 per frame, drawing disabled (diagnostic) | 161 µs | 8.6 ms | 69.5 | 1 112 |

What these figures say:

- **The bridge is not the limit.** With drawing disabled, a 512 KiB frame makes the round trip in
  8.6 ms and the page receives 69 of them per second (36 MB/s over the loopback).
- **Drawing is, and it is not fluid yet.** The frame rate stays near 5 per second whatever the
  computation costs, and the "round trip" then mostly measures the page's main thread waiting for the
  renderer. WebKit's web process reports `NvRmMemInitNvmap failed` and `failed to create dri2 screen`
  at start: it does not reach the Tegra GPU and renders WebGL in software. Drawing fewer pixels
  (`--pixelratio 1`) doubles the rate; it does not make it fluid. Hardware-accelerated rendering of
  WebKitGTK on Tegra is the open part of the Jetson qualification
  ([validation notes](validation/jetson-528.md), #528), and this demo is a reproducer for it.
- **The backend switch shows where it should.** At 256 generations per frame the GPU computes a frame
  in 27 ms where the CPU would need 280 ms; at one generation per frame it shows nothing, for the
  reason given above.

The system-browser path (a build without a native engine) was not measured.

## Not done yet

Kiosk use (full screen at start, a systemd user unit, a clean exit) waits for the rendering question
above. Zero-copy device buffers on the Tegra's unified memory, a multi-threaded CPU backend, the
other power modes and a self-hosted runner for the CUDA comparison are follow-ups of #530.
