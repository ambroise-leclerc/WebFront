// Offline verification with an independent decoder (Node's zlib, not fflate).
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
const root = new URL('../../../', import.meta.url);
const assets = [
    ['ReactRuntimeData', 'react/runtime.mjs'],
    ['BabelData', 'babel/babel.min.js'],
    ['JasmineJsData', 'jasmine/jasmine.js'],
    ['JasmineHtmlData', 'jasmine/jasmine-html.js'],
    ['JasmineCssData', 'jasmine/jasmine.css'],
    ['JasmineBootData', 'jasmine/boot.js'],
    ['JasmineFaviconData', 'jasmine/jasmine_favicon.png'],
];
for (const [name, source] of assets) {
    const header = readFileSync(new URL(`include/webfront/system/${name}.hpp`, root), 'utf8');
    const size = Number(header.match(/dataSize\{(\d+)\}/)[1]);
    const words = [...header.matchAll(/0x([0-9a-f]{16})/g)].map(match => match[1]);
    const storage = Buffer.from(words.join(''), 'hex');
    assert.equal(storage.length, Math.ceil(size / 8) * 8);
    assert(storage.subarray(size).every(byte => byte === 0), `${name}: word padding`);
    const payload = storage.subarray(0, size);
    const compressed = !source.endsWith('.png');
    assert(header.includes(compressed ? 'encoding{"gzip"}' : 'encoding{}'));
    if (compressed) {
        // CM=deflate, FLG=0 (no filename), MTIME=0, XFL=2, fixed OS=3.
        assert.deepEqual([...payload.subarray(0, 10)], [31, 139, 8, 0, 0, 0, 0, 0, 2, 3]);
    }
    assert.deepEqual(compressed ? gunzipSync(payload) : payload,
        readFileSync(new URL(`third_party/${source}`, root)), `${name}: decoded contents`);
    console.log(`${name}: ${size} served bytes, ${storage.length} stored bytes`);
}

// React's npm distribution is CommonJS. A plain export-star silently loses named ESM exports.
const runtime = await import('../../../third_party/react/runtime.mjs');
assert.equal(runtime.version, '19.3.1');
assert.equal(runtime.default.version, runtime.version);
for (const name of ['useState', 'useEffect', 'createRoot', 'hydrateRoot', 'jsx', 'jsxs'])
    assert.equal(typeof runtime[name], 'function', `React ESM export: ${name}`);
assert.equal(runtime.Fragment, runtime.jsx(runtime.Fragment, {}).type);
