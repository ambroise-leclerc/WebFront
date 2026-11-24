// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-18
//
// WebFront — tests/fuzz
// Role: libFuzzer entry point of the HTTP request parser.
// Thread: worker-safe | Alloc: unrestricted
// LLM: fuzz entry point; the parser must never read past the buffer

#include <webfront/http/HTTPServer.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view  input(reinterpret_cast<const char*>(data), size);
    webfront::http::Request request;
    try {
        // Exercise state transitions across receive boundaries as well as complete requests.
        const auto split = size / 2;
        if (!request.parseSomeData(input.begin(), input.begin() + split))
            request.parseSomeData(input.begin() + split, input.end());
        if (request.completed()) {
            (void)request.getMethodName();
            (void)request.isUpgradeRequest("websocket");
            (void)request.acceptsEncoding("gzip");
            (void)request.acceptsEncoding("identity");
        }
    } catch (const webfront::http::BadRequestException&) {
        // Invalid HTTP is an expected parser result.
    }
    return 0;
}
