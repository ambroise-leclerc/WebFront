#!/usr/bin/env bash
# Fails when the working tree differs from HEAD on a path outside the closed list of the release commit (ADR-021, decision 1),
# or when an untracked file outside the list exists (the files of the list may be changed or new). The list is the arguments: the release-developer workflow declares it once.
#   check-release-paths.sh <path>...
set -euo pipefail

[ "$#" -gt 0 ] || { echo "Usage: check-release-paths.sh <path>..." >&2; exit 2; }
status=0
while IFS= read -r -d '' changed; do
    allowed=false
    for path in "$@"; do
        if [ "$changed" = "$path" ]; then allowed=true; break; fi
    done
    if [ "$allowed" = false ]; then
        echo "::error::The release changes $changed, which is not in the closed list of paths" >&2
        status=1
    fi
done < <({ git diff --name-only -z HEAD; git ls-files --others --exclude-standard -z; })
exit "$status"
