// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-16
//
// WebFront — examples
// Role: CUDA unit of the host-loop example: the ray-traced Boing Ball computed on the device.
// Thread: worker-safe | Alloc: unrestricted
// LLM: optional CUDA build; device memory is allocated lazily and freed by cudaRelease

// The CUDA renderer of the Boing Ball: one thread per pixel runs the same pixel() as the CPU renderer.
// Compiled by nvcc at C++17; includes no WebFront header.

#include "BoingCuda.hpp"

#include <cuda_runtime.h>

namespace boing {
namespace {

__global__ void renderKernel(Scene scene, std::uint32_t* pixels) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x < scene.width && y < scene.height)
        pixels[y * scene.width + x] = pixel(scene, x, y);
}

std::uint32_t* devicePixels   = nullptr;
std::size_t    deviceCapacity = 0;

const char* failure(cudaError_t status) {
    return status == cudaSuccess ? nullptr : cudaGetErrorString(status);
}

}  // namespace

const char* cudaDevice() {
    static char    name[256] = "";
    int            count     = 0;
    cudaDeviceProp properties;
    if (name[0] == '\0' && cudaGetDeviceCount(&count) == cudaSuccess && count > 0 && cudaGetDeviceProperties(&properties, 0) == cudaSuccess)
        for (std::size_t i = 0; i + 1 < sizeof name && properties.name[i] != '\0'; ++i)
            name[i] = properties.name[i];
    return name;
}

const char* cudaCompute(const Scene& scene) {
    const auto count = static_cast<std::size_t>(scene.width) * static_cast<std::size_t>(scene.height);
    if (count > deviceCapacity) {
        cudaRelease();
        if (const char* error = failure(cudaMalloc(&devicePixels, count * sizeof(std::uint32_t))))
            return error;
        deviceCapacity = count;
    }
    const dim3 block(16, 16);
    const dim3 grid((static_cast<unsigned>(scene.width) + block.x - 1) / block.x, (static_cast<unsigned>(scene.height) + block.y - 1) / block.y);
    renderKernel<<<grid, block>>>(scene, devicePixels);
    if (const char* error = failure(cudaGetLastError()))
        return error;
    return failure(cudaDeviceSynchronize());
}

const char* cudaCopy(std::uint32_t* pixels, std::size_t count) {
    if (count > deviceCapacity)
        return "no computed image of that size";
    return failure(cudaMemcpy(pixels, devicePixels, count * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
}

void cudaRelease() {
    cudaFree(devicePixels);
    devicePixels   = nullptr;
    deviceCapacity = 0;
}

}  // namespace boing
