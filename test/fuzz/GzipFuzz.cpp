// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-19
//
// WebFront — tests/fuzz
// Role: libFuzzer entry point of the gzip decoder.
// Thread: worker-safe | Alloc: unrestricted
// LLM: fuzz entry point; the decoder must reject anything invalid without overrun

#include <webfront/http/Gzip.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Bound expansion even for valid compression bombs. Invalid input is an expected result.
    (void)webfront::http::gzip::decodeGzip(std::string_view(reinterpret_cast<const char*>(data), size), 1024 * 1024);
    return 0;
}
