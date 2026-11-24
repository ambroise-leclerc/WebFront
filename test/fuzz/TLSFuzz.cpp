// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-21
//
// WebFront — tests/fuzz
// Role: libFuzzer entry point of the TLS record and handshake parsers.
// Thread: worker-safe | Alloc: unrestricted
// LLM: fuzz entry point; parsers must never read past the buffer

#include <webfront/tls/detail/Protocol.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace tls = webfront::tls::detail;
    const tls::View input(data, size);
    try {
        tls::parseHello(input, false);
    } catch (const tls::Error&) { }
    if (size < 5)
        return 0;
    try {
        const auto header = input.first(5);
        const auto record = tls::parseRecord(header, true);
        if (record.type == tls::applicationData && record.size == size - 5) {
            tls::Traffic traffic;
            traffic.install(tls::zeros);
            tls::Bytes payload(input.begin() + 5, input.end());
            traffic.open(header, payload);
        }
    } catch (const tls::Error&) { }
    return 0;
}
