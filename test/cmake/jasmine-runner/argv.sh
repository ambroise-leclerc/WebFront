#!/bin/sh
# Stands in for a consumer application: records its arguments, one bracketed line each, and reports success.
: > "$OUT/argv.txt"
for argument in "$@"; do printf '[%s]\n' "$argument" >> "$OUT/argv.txt"; done
printf '{"status":"passed"}\n' > "$OUT/report.json"
