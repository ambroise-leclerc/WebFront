#!/usr/bin/env bash
# Accepts the version of a release of the developer environment: check-release-version.sh <requested> <sources> <pinned>.
# <sources> is the version of distribution.json and <pinned> the published one of DeveloperReleaseData.hpp. A pull request
# that changes the environment advances the sources ahead of the pin, so the requested version may be the one the sources
# already name; it is never below the sources, and always above the published version, which is immutable.
set -euo pipefail
requested=${1:?requested version} sources=${2:?version of the sources} pinned=${3:?pinned version}
for version in "$requested" "$sources" "$pinned"; do
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "::error::Version '$version' must be MAJOR.MINOR.PATCH"; exit 1; }
done
newest() { printf '%s\n' "$@" | sort -V | tail -n 1; }
if [ "$(newest "$sources" "$requested")" != "$requested" ]; then
  echo "::error::Version $requested must not be lower than the $sources of the sources"
  exit 1
fi
if [ "$requested" = "$pinned" ] || [ "$(newest "$pinned" "$requested")" != "$requested" ]; then
  echo "::error::Version $requested must be greater than the published $pinned"
  exit 1
fi
