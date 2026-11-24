// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-16
//
// WebFront — examples
// Role: Interface of the optional CUDA unit: device name, whole-image computation, copy back and release.
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: functions return nullptr or the reason of a failure; see docs/HOST_LOOP_EXAMPLE.md

#pragma once

// The optional CUDA renderer of the Boing Ball (BoingCuda.cu). Its interface is plain C++ so that the unit nvcc
// compiles includes no WebFront header. A build without CUDA (WEBFRONT_BOING_CUDA undefined) reports no device.

#include "BoingScene.hpp"

#include <cstddef>
#include <cstdint>

namespace boing {

#ifdef WEBFRONT_BOING_CUDA
/// The name of the CUDA device, or an empty string when no device can be used.
const char* cudaDevice();
/// Computes every pixel of `scene` in device memory and waits for the device: the whole computation, timed by the caller.
/// Returns nullptr, or the reason of the failure.
const char* cudaCompute(const Scene& scene);
/// Copies the `count` pixels of the last computed image to host memory. Returns nullptr, or the reason of the failure.
const char* cudaCopy(std::uint32_t* pixels, std::size_t count);
/// Frees the device memory; the next computation allocates it again.
void cudaRelease();
#else
inline const char* cudaDevice() {
    return "";
}
inline const char* cudaCompute(const Scene&) {
    return "this build has no CUDA renderer";
}
inline const char* cudaCopy(std::uint32_t*, std::size_t) {
    return "this build has no CUDA renderer";
}
inline void cudaRelease() { }
#endif

}  // namespace boing
