#!/usr/bin/env bash
# Check commits and, when supplied, PR_TITLE and PR_BODY.
# Usage: check-no-ai-attribution.sh <base> <head>
# An empty base checks all ancestors of head, including root commits.
set -euo pipefail

base="${1?base revision (empty for all history)}"
head="${2:?head revision}"

# POSIX ERE boundaries keep these patterns usable with GNU and BSD grep.
# A person's name alone is never an identity or co-author match. GitHub bot
# addresses must belong to a known AI tool, not an arbitrary noreply user/bot.
tool='(claude|copilot|copilot-swe-agent|codex|chatgpt|google-labs-jules|jules|gemini|vibe)'
github_tool="(${tool}\\[bot\\]|copilot|copilot-swe-agent|google-labs-jules)"
tool_email="(noreply@(anthropic|openai)\\.com|([0-9]+\\+)?${github_tool}@users\\.noreply\\.github\\.com)"
identity="<${tool_email}>|^${tool}\\[bot\\][[:space:]]*<|^(copilot-swe-agent|google-labs-jules)[[:space:]]*<"
trailer="^(co-authored-by|generated-by|assisted-by):.*<${tool_email}>|^claude-session:|claude\\.ai/code/session|generated (with|by)[[:space:]]+\\[?${tool}([^[:alnum:]_]|$)|assisted by[[:space:]]+\\[?${tool}([^[:alnum:]_]|$)|🤖 generated"

commits="$(mktemp)"
trap 'rm -f -- "$commits"' EXIT
revision="$head"
if [ -n "$base" ]; then
    revision="$base..$head"
fi
if ! git rev-list "$revision" >"$commits"; then
    echo "::error::unable to list the commits in $revision"
    exit 1
fi

status=0
while read -r sha; do
    ident="$(git show -s --format='%an <%ae>%n%cn <%ce>' "$sha")"
    message="$(git show -s --format='%B' "$sha")"
    if grep -qiE "$identity" <<<"$ident"; then
        echo "::error::$sha has an AI author or committer"
        status=1
    fi
    if grep -qiE "$trailer" <<<"$message"; then
        echo "::error::$sha message contains AI attribution"
        status=1
    fi
done <"$commits"

# Do not print untrusted PR text into workflow commands/logs.
for field in PR_TITLE PR_BODY; do
    if grep -qiE "$trailer" <<<"${!field-}"; then
        echo "::error::$field contains AI attribution"
        status=1
    fi
done

exit "$status"
