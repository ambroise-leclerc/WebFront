// Manual Release benchmark; global allocation counters are isolated from the test runtime.
#include "GzipFixtures.hpp"
#include <webfront/http/Gzip.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>

namespace {
std::size_t allocations{}, allocatedBytes{};
}

void* operator new(std::size_t size) {
    if (auto* memory = std::malloc(size ? size : 1)) {
        ++allocations;
        allocatedBytes += size;
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
void measure(std::string_view name, std::string_view bytes, bool reserved) {
    const auto expected = webfront::http::gzip::decodeGzip(bytes);
    if (!expected)
        throw std::runtime_error("invalid or over-budget benchmark input");
    const unsigned iterations = expected->size() > 1024 * 1024 ? 40 : 2000;
    std::string    output;
    if (reserved)
        output.reserve(expected->size());
    const auto initialCount = allocations, initialBytes = allocatedBytes;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned index = 0; index < iterations; ++index) {
        if (!reserved)
            std::string{}.swap(output);
        if (!webfront::http::gzip::decodeGzipInto(bytes, output, expected->size()) || output != *expected)
            throw std::runtime_error("benchmark decode mismatch");
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto count = allocations - initialCount, size = allocatedBytes - initialBytes;
    std::cout << name << ',' << (reserved ? "reserved" : "fresh") << ',' << bytes.size() << ',' << expected->size() << ',' << iterations << ','
              << elapsed * 1e6 / iterations << ',' << static_cast<double>(expected->size()) * iterations / elapsed / (1024 * 1024) << ','
              << static_cast<double>(count) / iterations << ',' << static_cast<double>(size) / iterations << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(12);
        std::cout << "asset,output,compressed_bytes,decoded_bytes,iterations,us_per_decode,MiB_per_s,allocations,allocated_bytes\n";
        for (const auto& [name, bytes] : {
                 std::pair{   "fixed",    gzipfixtures::fixed},
                 { "dynamic",  gzipfixtures::dynamic},
                 {"optional", gzipfixtures::optional}
        })
            for (const bool reserved : {false, true})
                measure(name, bytes, reserved);
        for (int index = 1; index < argc; ++index) {
            std::ifstream file(argv[index], std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot open benchmark input");
            const std::string bytes(std::istreambuf_iterator<char>(file), {});
            for (const bool reserved : {false, true})
                measure(argv[index], bytes, reserved);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
