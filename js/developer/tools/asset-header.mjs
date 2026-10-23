// The checked-in header format of an embedded asset, with no host API: wf scripts and the Node tools render the same bytes.
import {compress} from '../environment/embed.mjs';

const hexOf = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');

export function renderAsset(source, name, compressed = false) {
    const payload = compressed ? compress(source) : source;
    const hex = hexOf(payload);
    const lines = [];
    // Build bounded lines, then join once. Appending every word in CMake repeatedly
    // copied a growing string and made large assets particularly expensive on Windows.
    for (let offset = 0; offset < hex.length; offset += 80) {
        const words = hex.slice(offset, offset + 80).match(/.{1,16}/g);
        lines.push(`        ${words.map(word => `0x${word.padEnd(16, '0')}`).join(', ')}`);
    }
    const words = lines.length ? `\n${lines.join(',\n')}` : '';
    return `#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace webfront::fs::generated {

struct ${name} {
    static constexpr std::string_view encoding${compressed ? '{"gzip"}' : '{}'};
    static constexpr std::size_t dataSize{${payload.length}};
    static constexpr std::array<std::uint64_t, ${Math.ceil(payload.length / 8)}> data{${words}
    };
};

} // namespace webfront::fs::generated
`;
}
