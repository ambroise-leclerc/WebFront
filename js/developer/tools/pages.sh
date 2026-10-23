#!/usr/bin/env bash
# A token-driven branch push does not trigger Pages automatically: request the build explicitly.
# With --check, only verify the configured Pages source (before anything is pushed to gh-pages).
# Needs the gh CLI (GH_TOKEN) and GITHUB_REPOSITORY; no other tool.
set -euo pipefail

[[ "${GITHUB_REPOSITORY:-}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || { echo "GITHUB_REPOSITORY is required" >&2; exit 1; }

source=$(gh api "repos/$GITHUB_REPOSITORY/pages" --jq '[.build_type, .source.branch, .source.path] | join(" ")')
if [ "$source" != "legacy gh-pages /" ]; then
    echo "Configure GitHub Pages to build from gh-pages / before publishing; an existing site is never reconfigured automatically" >&2
    exit 1
fi
if [ "${1:-}" != "--check" ]; then
    gh api --method POST "repos/$GITHUB_REPOSITORY/pages/builds"
fi
