#!/usr/bin/env bash
# Trusted master or a maintainer-dispatched branch only. The service supplies PATH, CC/CXX and CUDAHOSTCXX where needed.
set -euo pipefail
umask 077
test "$(uname -m)" = aarch64
test -r /etc/nv_tegra_release
: "${WEBFRONT_JETSON_CACHE:?Set an absolute cache directory owned by the runner}"
[[ "$WEBFRONT_JETSON_CACHE" = /* ]]
export CCACHE_DIR="$WEBFRONT_JETSON_CACHE/ccache"
export CPM_SOURCE_CACHE="$WEBFRONT_JETSON_CACHE/cpm"
export CUDA_CACHE_PATH="$WEBFRONT_JETSON_CACHE/cuda"
export CUDA_CACHE_MAXSIZE=268435456
mkdir -p "$CCACHE_DIR" "$CPM_SOURCE_CACHE" "$CUDA_CACHE_PATH"
ccache --max-size 6G
# Never inherit a desktop display or a software-renderer override. CTest creates Xvfb.
unset DISPLAY WAYLAND_DISPLAY GSK_RENDERER LIBGL_ALWAYS_SOFTWARE WEBKIT_DISABLE_DMABUF_RENDERER
{
    echo '### Jetson Orin hardware validation'
    echo '```text'
    cat /etc/nv_tegra_release
    dpkg-query -W nvidia-jetpack nvidia-l4t-core 2>/dev/null || true
    uname -a
    "${CXX:-c++}" --version
    cmake --version
    pkg-config --modversion webkitgtk-6.0
    nvcc --version
    nvpmodel -q
    echo '```'
} >> "${GITHUB_STEP_SUMMARY:-/dev/stdout}"
# Fixed names, no user input; stale/interrupted builds are removed on the next job too.
rm -rf -- build-jetson-core build-jetson-webkitgtk
# CPM has no LRU. Clear only this dedicated cache above its 8 GiB high-water mark.
if (( $(du -sm "$CPM_SOURCE_CACHE" | cut -f1) > 8192 )); then
    find "$CPM_SOURCE_CACHE" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
fi
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_ENABLE_TESTING=ON
    -DWEBFRONT_EMBED_CEF=OFF -DWARNINGS_AS_ERRORS=OFF -DENABLE_CACHE=OFF
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
    -DWEBFRONT_HOST_LOOP_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=87)
cmake -S . -B build-jetson-core "${common[@]}" -DWEBFRONT_EMBED_WEBKITGTK=OFF
cmake --build build-jetson-core --parallel 2
# Select one scenario directly: Bdd exits 4 if it skips for lack of a GPU. CTest permits skips.
build-jetson-core/test/unit/tests 'Scenario: Host loop: CUDA computes the image of the CPU, within the rounding of the device'
ctest --test-dir build-jetson-core --output-on-failure --no-tests=error --parallel 2 \
    --output-junit core-results.xml
cmake -S . -B build-jetson-webkitgtk "${common[@]}" -DWEBFRONT_EMBED_WEBKITGTK=ON \
    -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK -DWEBFRONT_BUILD_EXAMPLES=OFF
cmake --build build-jetson-webkitgtk --target webtest developer_integration webfront-wf --parallel 2
ctest --test-dir build-jetson-webkitgtk -L web-integration --output-on-failure --no-tests=error \
    --parallel 2 --output-junit webkitgtk-results.xml
ccache --show-stats
