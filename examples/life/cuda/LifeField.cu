// CUDA backend of the Life demo. nvcc compiles this unit at its own language level; it sees LifeField.hpp only.
#include "LifeField.hpp"

#include <cuda_runtime.h>

#include <cstdio>
#include <mutex>
#include <utility>

namespace webfront::demo::lifecuda::gpu {
namespace {

    __global__ void stepKernel(const std::uint64_t* in, std::uint64_t* out, std::uint32_t side) {
        const std::uint32_t words = side / 64;
        const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
        if (index < words * side)
            out[index] = nextWord(in, side, index / words, index % words);
    }

    // The two device fields a generation reads and writes, kept between calls: allocating them costs more than a
    // generation. One mutex serializes every use of the device by this demo.
    struct DeviceFields {
        std::mutex     mutex;
        std::uint64_t* front = nullptr;
        std::uint64_t* back  = nullptr;
        std::size_t    words = 0;
    };

    DeviceFields& fields() {
        static DeviceFields instance;
        return instance;
    }

    void freeFields(DeviceFields& device) noexcept {
        if (device.front)
            cudaFree(device.front);
        if (device.back)
            cudaFree(device.back);
        device.front = device.back = nullptr;
        device.words               = 0;
    }

    bool fail(const char* step, cudaError_t status, char* error, std::size_t errorSize) noexcept {
        if (errorSize != 0)
            std::snprintf(error, errorSize, "%s: %s", step, cudaGetErrorString(status));
        return false;
    }

}  // namespace

bool available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

bool deviceName(char* name, std::size_t size) noexcept {
    cudaDeviceProp properties;
    if (size == 0 || !available() || cudaGetDeviceProperties(&properties, 0) != cudaSuccess)
        return false;
    std::snprintf(name, size, "%s", properties.name);
    return true;
}

bool advance(std::uint64_t* cells, std::uint32_t side, std::uint32_t generations, char* error, std::size_t errorSize) noexcept {
    auto&                 device = fields();
    const std::lock_guard lock{device.mutex};
    const std::size_t     words = fieldWords(side);
    const std::size_t     bytes = words * sizeof(std::uint64_t);
    if (device.words != words) {
        freeFields(device);
        if (const auto status = cudaMalloc(&device.front, bytes); status != cudaSuccess)
            return fail("cudaMalloc", status, error, errorSize);
        if (const auto status = cudaMalloc(&device.back, bytes); status != cudaSuccess) {
            freeFields(device);
            return fail("cudaMalloc", status, error, errorSize);
        }
        device.words = words;
    }
    if (const auto status = cudaMemcpy(device.front, cells, bytes, cudaMemcpyHostToDevice); status != cudaSuccess)
        return fail("upload", status, error, errorSize);
    constexpr unsigned threads = 256;
    const auto         blocks  = static_cast<unsigned>((words + threads - 1) / threads);
    for (std::uint32_t generation = 0; generation < generations; ++generation) {
        stepKernel<<<blocks, threads>>>(device.front, device.back, side);
        std::swap(device.front, device.back);
    }
    if (const auto status = cudaGetLastError(); status != cudaSuccess)
        return fail("kernel launch", status, error, errorSize);
    // The download waits for the queued generations and reports an error raised while they ran.
    if (const auto status = cudaMemcpy(cells, device.front, bytes, cudaMemcpyDeviceToHost); status != cudaSuccess)
        return fail("download", status, error, errorSize);
    return true;
}

void release() noexcept {
    auto&                 device = fields();
    const std::lock_guard lock{device.mutex};
    freeFields(device);
}

}  // namespace webfront::demo::lifecuda::gpu
