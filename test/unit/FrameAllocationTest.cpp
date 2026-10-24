// Regression (#363): with the Debug level disabled, adding a buffer to a WebSocket frame must not format it. The hex
// dump passed to log::debug() was built before the level was tested, on every buffer of every frame sent, and cost
// two thirds of a 32 KiB bridge call (ADR-017 measurements). The dump is not observable once disabled, so this program
// counts the bytes allocated: its own executable, because it replaces the global operator new.
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <vector>

namespace {
std::atomic<std::size_t> allocated{0};
}

#if defined(__GNUC__) && !defined(__clang__)
    // GCC pairs the inlined replacement new with free() and reports a mismatch the replacement makes consistent.
    #pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    allocated += size;
    if (auto* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    using namespace webfront;
    log::setLogLevel(log::Info);
    const std::vector<std::byte>                 payload(std::size_t{1} << 20, std::byte{0x2a});
    const std::vector<std::byte>                 header(2, std::byte{});
    websocket::Frame<networking::NetworkingMock> frame{std::span(header)};
    const auto                                   before = allocated.load();
    frame.addBuffer(std::span(payload));
    const auto used = allocated.load() - before;
    // Recording a span needs a few pointers; a dump of 1 MiB needs megabytes.
    if (used >= payload.size()) {
        std::cerr << "Frame::addBuffer allocated " << used << " bytes for a 1 MiB buffer with Debug disabled\n";
        return 1;
    }
    return 0;
}
