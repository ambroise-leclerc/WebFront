#!/usr/bin/env bash
# Offline regression tests; all commits live in an isolated temporary repository.
set -euo pipefail
checker="$(cd -- "$(dirname -- "$0")" && pwd)/check-no-ai-attribution.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
# Ignore the caller's Git configuration, hooks, signing and identity.
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_CONFIG_COUNT
unset PR_TITLE PR_BODY
export GIT_AUTHOR_NAME='Alice Example' GIT_AUTHOR_EMAIL='alice@example.org'
export GIT_COMMITTER_NAME='Alice Example' GIT_COMMITTER_EMAIL='alice@example.org'
git init -q "$work"
cd -- "$work"

# These checks inspect Git metadata, not the index/worktree or the porcelain commit command.
# commit-tree creates the same real commit chain without scanning the index or updating reflogs.
tree="$(git mktree </dev/null)"
commit() {
    local parent='' sha
    local arguments=(commit-tree "$tree" -m "$1")
    if [ -f "$work/parent" ]; then
        read -r parent <"$work/parent"
        arguments+=(-p "$parent")
    fi
    sha="$(git -c commit.gpgSign=false "${arguments[@]}")"
    printf '%s\n' "$sha" >"$work/parent"
    printf '%s\n' "$sha"
}

expect() {
    local expected="$1" label="$2" actual=0
    shift 2
    bash "$checker" "$@" >"$work/output" 2>&1 || actual=$?
    if [ "$actual" -ne "$expected" ]; then
        printf 'FAIL: %s (expected %s, got %s)\n' "$label" "$expected" "$actual"
        cat "$work/output"
        exit 1
    fi
}

root="$(commit 'Claude-Session: https://claude.ai/code/session_root')"
clean="$(commit 'Ordinary change')"
expect 1 'root included in full history' '' "$clean"
expect 0 'base excluded from range' "$root" "$clean"
expect 0 'empty range' "$clean" "$clean"
expect 1 'invalid revision fails' "$clean" missing-revision

# Signed commits must have exactly the same record framing under user log settings.
# A fixture verifier exercises Git's signature display without keys or network access.
signed() {
    printf 'tree %s\nparent %s\nauthor Alice Example <alice@example.org> 1700000000 +0000\ncommitter Alice Example <alice@example.org> 1700000000 +0000\ngpgsig -----BEGIN PGP SIGNATURE-----\n invalid\n -----END PGP SIGNATURE-----\n\n%s\n' "$tree" "$1" "$2" |
        git hash-object -t commit -w --stdin
}
cat >"$work/verify-signature" <<'VERIFY'
#!/usr/bin/env bash
: >"${0%/*}/signature-checked"
cat >/dev/null
printf 'gpg: Good signature from "Alice Example"\n' >&2
printf '[GNUPG:] GOODSIG 0123456789ABCDEF Alice Example\n'
printf '[GNUPG:] VALIDSIG 0123456789ABCDEF0123456789ABCDEF01234567 2023-11-14 1700000000 0 4 0 1 10 00 0123456789ABCDEF0123456789ABCDEF01234567\n'
VERIFY
chmod +x "$work/verify-signature"
git config gpg.program "$work/verify-signature"
git config log.showSignature true
git config log.decorate full
git config log.showNotes true
git config format.pretty oneline
signed_clean="$(signed "$clean" 'Ordinary signed change')"
expect 0 'signed clean commit with user log settings' "$clean" "$signed_clean"
signed_dirty="$(signed "$signed_clean" 'Generated-by: Codex <codex@openai.com>')"
expect 1 'signed attribution with user log settings' "$clean" "$signed_dirty"
# A second signed record verifies that the parser stays aligned across the range.
signed_after="$(signed "$signed_dirty" 'Another ordinary signed change')"
expect 1 'signed attribution before a clean commit' "$clean" "$signed_after"
if [ -e "$work/signature-checked" ]; then
    echo 'FAIL: user signature display invoked the verifier during metadata parsing'
    exit 1
fi
git config --unset gpg.program
git config --unset log.showSignature
git config --unset log.decorate
git config --unset log.showNotes
git config --unset format.pretty

for name in 'Claude Martin' 'Jules Dupont' 'Gemini Rossi' 'Vibe Hansen' 'Copilot Smith'; do
    sha="$(GIT_AUTHOR_NAME="$name" GIT_COMMITTER_NAME="$name" commit 'Ordinary change')"
    expect 0 "human author and committer: $name" "$clean" "$sha"
    clean="$sha"
    sha="$(commit "Co-authored-by: $name <person@example.org>")"
    expect 0 "human co-author: $name" "$clean" "$sha"
    clean="$sha"
done

for email in noreply@anthropic.com claude@anthropic.com claude.opus+agent-test@anthropic.com noreply@openai.com vibe@mistral.ai gemini@google.com codex@openai.com chatgpt@openai.com '123+copilot[bot]@users.noreply.github.com' google-labs-jules@users.noreply.github.com; do
    sha="$(GIT_AUTHOR_EMAIL="$email" commit 'Ordinary change')"
    expect 1 "tool author: $email" "$clean" "$sha"
    clean="$sha"
    sha="$(GIT_COMMITTER_EMAIL="$email" commit 'Ordinary change')"
    expect 1 "tool committer: $email" "$clean" "$sha"
    clean="$sha"
    for key in Co-authored-by Generated-by Assisted-by; do
        sha="$(commit "$key: Tool <$email>")"
        expect 1 "tool trailer: $key $email" "$clean" "$sha"
        clean="$sha"
    done
done
# Canonical tool names with provider addresses; domains alone are not enough.
for ident in 'Claude Opus <claude@anthropic.com>' 'Mistral Vibe <vibe@mistral.ai>' 'Vibe <agent@mistral.ai>' 'Gemini <agent@google.com>' 'Codex <agent@openai.com>' 'ChatGPT <agent@openai.com>'; do
    name="${ident% <*}"
    email="${ident##*<}"
    email="${email%>}"
    sha="$(GIT_AUTHOR_NAME="$name" GIT_AUTHOR_EMAIL="$email" commit 'Ordinary change')"
    expect 1 "provider tool author: $ident" "$clean" "$sha"
    clean="$sha"
    sha="$(GIT_COMMITTER_NAME="$name" GIT_COMMITTER_EMAIL="$email" commit 'Ordinary change')"
    expect 1 "provider tool committer: $ident" "$clean" "$sha"
    clean="$sha"
    for key in Co-Authored-By Generated-by Assisted-by; do
        message="$key: $ident"
        sha="$(commit "$message")"
        expect 1 "provider tool trailer: $message" "$clean" "$sha"
        clean="$sha"
        PR_TITLE="$message" expect 1 'provider tool PR title' "$clean" "$clean"
        PR_BODY="$message" expect 1 'provider tool PR body' "$clean" "$clean"
    done
done
for ident in 'Gemini Rossi <gemini.rossi@google.com>' 'Vibe Hansen <vibe.hansen@mistral.ai>' 'Claude Martin <claude@openai.com>' 'Gemini <gemini@example.org>' 'Mistral Vibe <vibe@mistral.ai.example.org>'; do
    name="${ident% <*}"
    email="${ident##*<}"
    email="${email%>}"
    sha="$(GIT_AUTHOR_NAME="$name" GIT_AUTHOR_EMAIL="$email" GIT_COMMITTER_NAME="$name" GIT_COMMITTER_EMAIL="$email" commit "Co-authored-by: $ident")"
    expect 0 "human or unrelated provider identity: $ident" "$clean" "$sha"
    PR_BODY="Co-authored-by: $ident" expect 0 'human PR co-author at provider' "$clean" "$sha"
    clean="$sha"
done
for name in 'Claude[bot]' copilot-swe-agent google-labs-jules; do
    sha="$(GIT_AUTHOR_NAME="$name" commit 'Ordinary change')"
    expect 1 "tool name: $name" "$clean" "$sha"
    clean="$sha"
done
for email in 'claude@anthropic.com.example.org' 'claude@notanthropic.com' 'claude@example.org' 'claude@users.noreply.github.com.example.org' '123+dependabot[bot]@users.noreply.github.com' 'jules.dupont@users.noreply.github.com' 'claude@users.noreply.github.com' 'jules@users.noreply.github.com' 'gemini@users.noreply.github.com' 'vibe@users.noreply.github.com'; do
    sha="$(GIT_AUTHOR_EMAIL="$email" commit "Co-authored-by: Claude Martin <$email>")"
    expect 0 "unrelated address: $email" "$clean" "$sha"
    clean="$sha"
done
for message in 'Claude-Session: opaque-value' 'See https://claude.ai/code/session_123' 'Generated with [Claude](https://claude.ai)' 'Generated by Copilot' 'Generated by Codex.' 'Generated by [Vibe](https://example.org)' 'Assisted by Codex' '🤖 Generated'; do
    sha="$(commit "$message")"
    expect 1 "commit message: $message" "$clean" "$sha"
    clean="$sha"
    PR_TITLE="$message" expect 1 'PR title' "$clean" "$clean"
    PR_BODY="$message" expect 1 'PR body' "$clean" "$clean"
done
for message in 'Document the code generated by codex-gen tool' 'Parser output generated by Vibe-coded script' 'Document output assisted by codex-gen tool' 'Document output generated with Vibe-coded script'; do
    sha="$(commit "$message")"
    expect 0 "hyphenated tool reference: $message" "$clean" "$sha"
    clean="$sha"
    PR_TITLE="$message" expect 0 'hyphenated tool in PR title' "$clean" "$clean"
    PR_BODY="$message" expect 0 'hyphenated tool in PR body' "$clean" "$clean"
done
PR_BODY=$'Details\n\nCo-authored-by: Claude <noreply@anthropic.com>' expect 1 'PR multiline trailer' "$clean" "$clean"
PR_TITLE='Fix Claude and Jules examples' PR_BODY='Co-authored-by: Jules Dupont <jules@example.org>' expect 0 'human PR metadata' "$clean" "$clean"
PR_BODY='Literal $(touch injected) and `touch injected`' expect 0 'PR metadata is data' "$clean" "$clean"
test ! -e injected
printf 'Attribution checker regression tests passed.\n'
