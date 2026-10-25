"""Reproducible gzip benchmark/differential corpus; writes only to a new directory."""
import hashlib
import pathlib
import random
import sys
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=False)


def write(name, data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    compressor = zlib.compressobj(level, zlib.DEFLATED, 31, 8, strategy)
    compressed = compressor.compress(data) + compressor.flush()
    (out / (name + ".gz")).write_bytes(compressed)
    print(name, len(compressed), len(data), hashlib.sha256(compressed).hexdigest())


for name, path in [("bridge", root / "js/core/WebFront.js"),
                   ("bootstrap", root / "js/core/bootstrap/bootstrap.bundle.mjs")]:
    write(name, path.read_bytes())
# Representative application bundle supplied separately, e.g. the pinned developer compiler.mjs.
if len(sys.argv) > 2:
    write("application", pathlib.Path(sys.argv[2]).read_bytes())
size = 4 * 1024 * 1024 - 1
rng = random.Random(282)
noise = rng.randbytes(size)
pattern = b"const state = {value: 42}; export function render() { return state.value; }\n"
text = (pattern * (size // len(pattern) + 1))[:size]
write("near-stored", noise, 0)
write("near-fixed", text, strategy=zlib.Z_FIXED)
write("near-dynamic", text)
write("near-incompressible", noise)
write("near-huffman", bytes(random.Random(282).choices(range(16), k=size)),
      strategy=zlib.Z_HUFFMAN_ONLY)
compressor = zlib.compressobj(wbits=31)
mixed = compressor.compress(text[:4096]) + compressor.flush(zlib.Z_SYNC_FLUSH)
mixed += compressor.compress(noise[:513]) + compressor.flush()
(out / "mixed-blocks.gz").write_bytes(mixed)
print("mixed-blocks", len(mixed), 4096 + 513, hashlib.sha256(mixed).hexdigest())
for length in [0, 1, 2, 7, 8, 15, 16, 31, 32, 255, 256, 257, 258, 32767, 32768, 65535, 65536]:
    for level, strategy, name in [(0, 0, "stored"), (6, zlib.Z_FIXED, "fixed"), (6, 0, "dynamic")]:
        write(f"boundary-{length}-{name}", noise[:length], level, strategy)
