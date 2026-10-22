"""Regenerates the fake npm registry served to test/web/js/tools-specs.mjs: the tarball of the
webfront-test-tool package and the packuments that point at it. Run from any directory:

    python3 test/web/registry/make_fixture.py

The output is deterministic (no timestamps), so an unchanged script leaves the files unchanged.
"""

import base64
import gzip
import hashlib
import io
import json
import pathlib
import tarfile

HERE = pathlib.Path(__file__).resolve().parent
NAME = "webfront-test-tool"
VERSION = "1.0.0"
TARBALL = f"tarballs/{NAME}-{VERSION}.tgz"

package_json = {
    "name": NAME,
    "version": VERSION,
    "description": "Fixture of the WebFront browser integration test",
    "keywords": ["webfront-tool"],
    "webfront": {"kind": "tool", "title": "Test tool", "entry": "dist/index.html"},
}

files = {
    "package.json": json.dumps(package_json, indent=2).encode() + b"\n",
    "dist/index.html": b'<!DOCTYPE html>\n<title>Test tool</title>\n<p id="marker">webfront-test-tool page</p>\n',
    # Larger than a writeFile chunk (16384 bytes), so it is sent in several chunks.
    "dist/data.bin": bytes(index % 251 for index in range(40000)),
    # Longer than the 100-byte ustar name field: split between the ustar prefix and name fields.
    "dist/assets/" + "nested-directory-with-a-long-name/" * 3 + "file.txt": b"long path\n",
    # Non-ASCII: stored in a pax extended header.
    "dist/café.txt": b"pax path\n",
}


def tarball():
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for path, data in files.items():
            info = tarfile.TarInfo("package/" + path)
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            # node-tar, which npm uses, also writes plain ustar headers when a path fits in them.
            tar.format = tarfile.PAX_FORMAT if not path.isascii() else tarfile.USTAR_FORMAT
            tar.addfile(info, io.BytesIO(data))
    compressed = io.BytesIO()
    with gzip.GzipFile(fileobj=compressed, mode="wb", mtime=0) as stream:
        stream.write(archive.getvalue())
    return compressed.getvalue()


def packument(name, integrity):
    return {
        "name": name,
        "dist-tags": {"latest": VERSION},
        "versions": {VERSION: {"name": name, "version": VERSION, "dist": {"tarball": TARBALL, "integrity": integrity}}},
    }


def write_json(path, value):
    (HERE / path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


data = tarball()
(HERE / "tarballs").mkdir(exist_ok=True)
(HERE / TARBALL).write_bytes(data)
integrity = "sha512-" + base64.b64encode(hashlib.sha512(data).digest()).decode()
write_json(NAME, packument(NAME, integrity))
# Same tarball, wrong hash: the installation must be refused.
write_json("webfront-test-corrupt", packument("webfront-test-corrupt", "sha512-" + base64.b64encode(bytes(64)).decode()))
write_json(
    "search.json",
    {"objects": [{"package": {"name": NAME, "version": VERSION, "description": package_json["description"]}}], "total": 1},
)
