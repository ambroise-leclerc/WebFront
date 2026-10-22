// Keep the checked-in header format and the pinned compressor independent of the host.
import { gzipSync } from '../../../third_party/fflate/browser.mjs';
import { mkdirSync, readFileSync, realpathSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

export const STALE_ASSET_MESSAGE = 'is stale; run the webfront-embedded-assets target';

export function renderAsset(source, name, compressed = false) {
    const payload = compressed
        ? Buffer.from(gzipSync(source, { level: 9, mem: 8, mtime: 0 }))
        : source;
    const hex = payload.toString('hex');
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

function main() {
    const { values } = parseArgs({ options: {
        input: { type: 'string' }, struct: { type: 'string' },
        output: { type: 'string' }, check: { type: 'string' }, gzip: { type: 'boolean' },
    } });
    if (!values.input || !values.struct || (!values.output && !values.check))
        throw new Error('Supply --input, --struct and --output or --check');
    const contents = renderAsset(readFileSync(values.input), values.struct, values.gzip);
    if (values.check) {
        if (!readFileSync(values.check).equals(Buffer.from(contents)))
            throw new Error(`${values.check} ${STALE_ASSET_MESSAGE}`);
    } else {
        mkdirSync(dirname(values.output), { recursive: true });
        writeFileSync(values.output, contents);
    }
}

if (process.argv[1] && import.meta.url === pathToFileURL(realpathSync(resolve(process.argv[1]))).href) {
    try {
        main();
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
