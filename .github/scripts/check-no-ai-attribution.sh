#!/usr/bin/env bash
# Fails when a commit in the given range carries AI-assistance attribution.
# Usage: check-no-ai-attribution.sh <base> <head>   (checks commits in base..head)
set -euo pipefail

base="${1:?base revision}"
head="${2:?head revision}"

identity='(anthropic\.com|noreply@openai\.com|\bcopilot\b|\bcodex\b|google-labs-jules|\bclaude\b)'
trailer='^(co-authored-by|generated-by|assisted-by):.*(claude|anthropic|copilot|codex|openai|jules|gemini|\bvibe\b)|generated with \[?claude|🤖 generated'

commits="$(mktemp)"
trap 'rm -f -- "$commits"' EXIT
if ! git rev-list "$base..$head" >"$commits"; then
    echo "::error::unable to list the commits in $base..$head"
    exit 1
fi

status=0
while read -r sha; do
    ident="$(git show -s --format='%an <%ae>%n%cn <%ce>' "$sha")"
    message="$(git show -s --format='%B' "$sha")"
    if grep -qiE "$identity" <<<"$ident"; then
        echo "::error::$sha has an AI author or committer: $(tr '\n' ';' <<<"$ident")"
        status=1
    fi
    if grep -qiE "$trailer" <<<"$message"; then
        echo "::error::$sha message contains AI attribution"
        status=1
    fi
done <"$commits"

exit "$status"
