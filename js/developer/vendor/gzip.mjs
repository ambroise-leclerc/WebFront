// Pure-JavaScript DEFLATE avoids platform-dependent zlib streams. No npm install needed.
import { gzipSync } from '../../../third_party/fflate/browser.mjs';
import { readFileSync, writeFileSync } from 'node:fs';
const [input, output] = process.argv.slice(2);
const compressed = gzipSync(readFileSync(input), { level: 9, mem: 8, mtime: 0 });
// fflate's gzip header is fixed (OS = Unix), with no filename and no timestamp.
writeFileSync(output, compressed);
