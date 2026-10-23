#!/usr/bin/env bash
# Dedicated prereleases: retries verify existing assets, never replace an existing release.
#   archive-release.sh <developer-v<version>> <distribution directory>
# Needs the gh CLI (GH_TOKEN), jq and GITHUB_REPOSITORY; the tag must exist on the remote.
set -euo pipefail

tag=${1:?Usage: archive-release.sh <tag> <distribution directory>}
distribution=${2:?Usage: archive-release.sh <tag> <distribution directory>}
root=$(cd "$(dirname "$0")/../../.." && pwd)
version=$(jq -r .version "$root/js/developer/tools/distribution.json")
if [ "$tag" != "developer-v$version" ] || ! [[ "${GITHUB_REPOSITORY:-}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
    echo "Expected the dedicated distribution tag and GITHUB_REPOSITORY" >&2
    exit 1
fi

# The published files: the environment, each starter, the provisioning data and the pinned header. jq must succeed before
# anything is sent to GitHub: a status inside a process substitution would be lost.
if ! listed=$(jq -er '["environment.tgz"] + [.starters[].archive] + ["provision.json", "DeveloperReleaseData.hpp"] | .[]' \
        "$distribution/provision.json"); then
    echo "Cannot read the starters of $distribution/provision.json" >&2
    exit 1
fi
mapfile -t files <<< "$listed"
[ "${#files[@]}" -ge 4 ] || { echo "The distribution lists too few files" >&2; exit 1; }
for name in "${files[@]}"; do
    [ -f "$distribution/$name" ] || { echo "Missing distribution file $name" >&2; exit 1; }
done

errors=$(mktemp)
directory=$(mktemp -d)
trap 'rm -rf "$errors" "$directory"' EXIT
existing=false
if gh api "repos/$GITHUB_REPOSITORY/releases/tags/$tag" > /dev/null 2> "$errors"; then
    existing=true
elif ! grep -q '(HTTP 404)' "$errors"; then
    cat "$errors" >&2
    exit 1
fi

if [ "$existing" = true ]; then
    patterns=()
    for name in "${files[@]}"; do patterns+=(--pattern "$name"); done
    gh release download "$tag" --repo "$GITHUB_REPOSITORY" --dir "$directory" "${patterns[@]}"
    for name in "${files[@]}"; do
        cmp -s "$directory/$name" "$distribution/$name" || { echo "Existing release asset $name differs; choose a new version" >&2; exit 1; }
    done
    echo "Existing release assets match the distribution"
else
    paths=()
    for name in "${files[@]}"; do paths+=("$distribution/$name"); done
    gh release create "$tag" --repo "$GITHUB_REPOSITORY" --verify-tag --prerelease \
        --title "WebFront developer packages $version" --notes-file "$root/js/developer/tools/RELEASE_NOTES.md" "${paths[@]}"
fi
