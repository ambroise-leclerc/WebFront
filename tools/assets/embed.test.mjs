import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';
import { test } from 'node:test';
import { renderAsset, STALE_ASSET_MESSAGE } from './embed.mjs';

test('header preserves word order, padding, line wrapping and empty arrays', () => {
    const source = Buffer.from(Array.from({ length: 41 }, (_, i) => i));
    assert.equal(renderAsset(source, 'Fixture'), `#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace webfront::fs::generated {

struct Fixture {
    static constexpr std::string_view encoding{};
    static constexpr std::size_t dataSize{41};
    static constexpr std::array<std::uint64_t, 6> data{
        0x0001020304050607, 0x08090a0b0c0d0e0f, 0x1011121314151617, 0x18191a1b1c1d1e1f, 0x2021222324252627,
        0x2800000000000000
    };
};

} // namespace webfront::fs::generated
`);
    for (const size of [0, 1, 7, 8, 9, 39, 40, 41]) {
        const header = renderAsset(source.subarray(0, size), 'Fixture');
        assert(header.includes(`dataSize{${size}}`));
        assert(header.includes(`std::array<std::uint64_t, ${Math.ceil(size / 8)}> data{`));
        const words = [...header.matchAll(/0x([0-9a-f]{16})/g)].map(match => match[1]);
        const decoded = Buffer.from(words.join(''), 'hex');
        assert.deepEqual(decoded.subarray(0, size), source.subarray(0, size));
        assert(decoded.subarray(size).every(byte => byte === 0));
        if (size === 0) assert(header.includes('data{\n    };'));
    }
});

test('gzip is deterministic and decodes independently for binary and empty inputs', () => {
    for (const source of [Buffer.alloc(0), Buffer.from(Array.from({ length: 256 }, (_, i) => i))]) {
        const header = renderAsset(source, 'Fixture', true);
        assert.equal(header, renderAsset(source, 'Fixture', true));
        assert(header.includes('encoding{"gzip"}'));
        const size = Number(header.match(/dataSize\{(\d+)\}/)[1]);
        const hex = [...header.matchAll(/0x([0-9a-f]{16})/g)].map(match => match[1]).join('');
        const payload = Buffer.from(hex, 'hex').subarray(0, size);
        assert.deepEqual([...payload.subarray(0, 10)], [31, 139, 8, 0, 0, 0, 0, 0, 2, 3]);
        assert.deepEqual(gunzipSync(payload), source);
    }
});

test('CMake entry point checks exact bytes and returns nonzero for stale or missing files', () => {
    const directory = mkdtempSync(join(tmpdir(), 'webfront-embed-'));
    try {
        const input = join(directory, 'input with spaces.bin');
        const output = join(directory, 'nested', 'Fixture.hpp');
        const script = fileURLToPath(new URL('../../cmake/EmbedTextAsset.cmake', import.meta.url));
        const source = Buffer.from([0, 128, 255, 13, 10]);
        writeFileSync(input, source);
        const run = (...args) => spawnSync(process.env.WEBFRONT_TEST_CMAKE || 'cmake',
            [`-DINPUT=${input}`, '-DSTRUCT=Fixture', ...args, '-P', script],
            { cwd: directory, encoding: 'utf8' });
        for (const compressed of [false, true]) {
            const gzip = `-DGZIP=${compressed ? 'ON' : 'OFF'}`;
            let result = run(gzip, `-DOUTPUT=${output}`);
            assert.equal(result.status, 0, result.stderr);
            const expected = renderAsset(source, 'Fixture', compressed);
            assert.equal(readFileSync(output, 'utf8'), expected);
            result = run(gzip, `-DCHECK=${output}`);
            assert.equal(result.status, 0, result.stderr);
            writeFileSync(output, expected.replaceAll('\n', '\r\n'));
            result = run(gzip, `-DCHECK=${output}`);
            assert.equal(result.status, 1, result.stderr);
            assert(result.stderr.includes(STALE_ASSET_MESSAGE), result.stderr);
            writeFileSync(output, expected);
            writeFileSync(input, Buffer.from('changed'));
            result = run(gzip, `-DCHECK=${output}`);
            assert.equal(result.status, 1, result.stderr);
            assert(result.stderr.includes(STALE_ASSET_MESSAGE), result.stderr);
            writeFileSync(input, source);
        }
        rmSync(output);
        assert.notEqual(run(`-DCHECK=${output}`).status, 0);
        assert.notEqual(run().status, 0);
        rmSync(input);
        assert.notEqual(run(`-DOUTPUT=${output}`).status, 0);
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
});

test('CMake generates and checks assets through a symlinked checkout', () => {
    const directory = mkdtempSync(join(tmpdir(), 'webfront-embed-symlink-'));
    try {
        const checkout = join(directory, 'linked checkout');
        symlinkSync(fileURLToPath(new URL('../../', import.meta.url)), checkout,
            process.platform === 'win32' ? 'junction' : 'dir');
        const input = join(directory, 'input.bin');
        const output = join(directory, 'Fixture.hpp');
        const source = Buffer.from([0, 128, 255, 13, 10]);
        writeFileSync(input, source);
        const run = (...args) => spawnSync(process.env.WEBFRONT_TEST_CMAKE || 'cmake',
            [`-DINPUT=${input}`, '-DSTRUCT=Fixture', ...args, '-P',
                join(checkout, 'cmake', 'EmbedTextAsset.cmake')],
            { cwd: directory, encoding: 'utf8' });
        for (const compressed of [false, true]) {
            const gzip = `-DGZIP=${compressed ? 'ON' : 'OFF'}`;
            writeFileSync(output, 'stale header');
            let result = run(gzip, `-DCHECK=${output}`);
            assert.equal(result.status, 1, result.stderr);
            assert(result.stderr.includes(STALE_ASSET_MESSAGE), result.stderr);
            rmSync(output);
            result = run(gzip, `-DOUTPUT=${output}`);
            assert.equal(result.status, 0, result.stderr);
            assert.equal(readFileSync(output, 'utf8'), renderAsset(source, 'Fixture', compressed));
            result = run(gzip, `-DCHECK=${output}`);
            assert.equal(result.status, 0, result.stderr);
        }
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
});

test('CLI reports a stale header on one line without a Node stack trace', () => {
    const directory = mkdtempSync(join(tmpdir(), 'webfront-embed-error-'));
    try {
        const input = join(directory, 'input.bin');
        const output = join(directory, 'Fixture.hpp');
        writeFileSync(input, 'source');
        writeFileSync(output, 'stale header');
        const result = spawnSync(process.execPath,
            [fileURLToPath(new URL('./embed.mjs', import.meta.url)),
                '--input', input, '--struct', 'Fixture', '--check', output],
            { encoding: 'utf8' });
        assert.equal(result.status, 1);
        assert.equal(result.stdout, '');
        assert.equal(result.stderr.trimEnd(), `${output} ${STALE_ASSET_MESSAGE}`);
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
});
