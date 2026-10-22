import {gzipSync} from '../../../third_party/fflate/browser.mjs';

export function compress(bytes) {
    return gzipSync(bytes, {level: 9, mem: 8, mtime: 0});
}

const byName = ([a], [b]) => a < b ? -1 : a > b ? 1 : 0;
const hasUnsafePart = path => path.split('/').some(part => !part || part === '.' || part === '..');

function validRoute(path) {
    if (!/^[A-Za-z0-9_./-]+$/.test(path)) return false;
    return !hasUnsafePart(path);
}

// Eight bytes as one big-endian word; the last word is padded with zeros.
const word = (bytes, offset) => '0x' + Array.from({length: 8}, (_, i) => (bytes[offset + i] ?? 0).toString(16).padStart(2, '0')).join('') + 'ULL';

function wordsOf(bytes) {
    const words = [];
    for (let offset = 0; offset < bytes.length; offset += 8) words.push(word(bytes, offset));
    return words;
}

function declarationOf(name, compressed) {
    const words = wordsOf(compressed);
    return `    struct ${name} {
        static constexpr std::string_view encoding{"gzip"};
        static constexpr std::size_t dataSize{${compressed.length}};
        static constexpr std::array<std::uint64_t, ${words.length}> data{\n            ${words.join(',\n            ')}\n        };
    };`;
}

// The same big-endian word layout, padding and gzip settings as EmbedTextAsset.cmake.
export function generateProvider(files, className = 'ApplicationAssets', notices = '') {
    if (!/^[A-Za-z][A-Za-z0-9_]*$/.test(className)) throw new Error('Invalid generated provider name');
    const declarations = [];
    const routes = [];
    for (const [path, bytes] of [...files].sort(byName)) {
        if (!validRoute(path)) throw new Error(`Invalid embedded route: ${path}`);
        const name = `Asset${declarations.length}`;
        declarations.push(declarationOf(name, compress(bytes)));
        routes.push(`        if (name == ${JSON.stringify(path)}) return File{${name}{}};`);
    }
    return `#pragma once

#include <webfront/system/FileSystem.hpp>

/* Dependency notices and build inputs\n${notices.replaceAll('*/', '* /')}\n*/
namespace webfront::fs {
class ${className} {
public:
    explicit ${className}(std::filesystem::path) {}
    static std::optional<File> open(std::filesystem::path path) {
        const auto name = path.relative_path().generic_string();
${routes.join('\n')}
        return {};
    }
private:
${declarations.join('\n')}
};
} // namespace webfront::fs
`;
}

export function applicationPage(files) {
    const encode = new TextEncoder();
    const stylesheet = files.has('app.css') ? '<link rel="stylesheet" href="./app.css">' : '';
    return encode.encode(`<!doctype html>\n<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>WebFront application</title>${stylesheet}<div id="root"></div><script src="/WebFront.js"></script><script type="module" src="./app.js"></script></html>\n`);
}
