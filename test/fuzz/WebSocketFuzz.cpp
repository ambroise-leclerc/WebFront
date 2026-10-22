#include <webfront/http/WebSocket.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto                              input = std::as_bytes(std::span(data, size));
    webfront::websocket::FrameDecoder decoder;
    const auto                        budget = [](auto) {
        return std::size_t{65536};
    };
    // The same bytes are decoded in one receive and in short chunks. Huge advertised
    // lengths are rejected by the production decoder's budget before any allocation.
    for (const auto chunkSize : {size, std::size_t{1}, std::size_t{7}}) {
        decoder.reset();
        auto remaining = input;
        while (!remaining.empty()) {
            const auto chunk    = remaining.first(std::min(chunkSize, remaining.size()));
            const bool complete = decoder.parse(chunk, budget);
            if (decoder.consumed() > chunk.size())
                std::abort();
            if (decoder.rejected())
                break;
            if (decoder.consumed() == 0)
                std::abort();
            remaining = remaining.subspan(decoder.consumed());
            if (complete) {
                (void)decoder.payload();
                decoder.reset();
            }
        }
    }
    return 0;
}
