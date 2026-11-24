// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-21
//
// WebFront — tests/unit
// Role: Valgrind check that private TLS inputs never drive a branch or an address.
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: run under Memcheck; marks only the private inputs undefined

#include <webfront/tls/detail/Curves.hpp>

#include <valgrind/memcheck.h>
namespace {
using namespace webfront::tls::detail;

[[gnu::noinline]] auto runP256(const Integer& k) {
    return p256Base(k);
}
[[gnu::noinline]] auto runX25519(const Digest& k) {
    return x25519(k, Digest{9});
}

}  // namespace

int main(int argc, char**) {
    using namespace webfront::tls::detail;
    Integer scalar{static_cast<std::uint32_t>(argc), 0xbce6faad, 0x01010101};
    Digest  key{static_cast<Byte>(argc), 0xab, 0x1e, 0xff, 0xc2};
    for (unsigned i = 0; i != 10; ++i) {
        scalar[2] ^= 1u << i;
        key[i]    ^= static_cast<Byte>(1 + i);
        VALGRIND_MAKE_MEM_UNDEFINED(scalar.data(), sizeof(scalar));
        VALGRIND_MAKE_MEM_UNDEFINED(key.data(), sizeof(key));
        auto point     = runP256(scalar);
        auto publicKey = runX25519(key);
        VALGRIND_MAKE_MEM_DEFINED(scalar.data(), sizeof(scalar));
        VALGRIND_MAKE_MEM_DEFINED(key.data(), sizeof(key));
        VALGRIND_MAKE_MEM_DEFINED(point.data(), sizeof(point));
        VALGRIND_MAKE_MEM_DEFINED(publicKey.data(), sizeof(publicKey));
        if (!nonzero(point[0]) || equalSecret(publicKey, Digest{}))
            return 1;
    }
}
