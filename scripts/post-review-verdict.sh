#!/usr/bin/env bash
# Post the Claude review verdict comment for a commit (ported from Josh-Wentworth/imagineer).
#
# The WORKFLOW posts this, from the model's schema-enforced structured output. The model never posts
# it: making the model responsible for producing the evidence that gates the model is the failure this
# whole design replaces (it reviewed for 24 turns, its `gh pr comment` was denied, and the result was a
# green check with no evidence).
#
# Two rules, both load-bearing:
#   1. NEVER post a verdict we did not actually receive. A fabricated "no issues" would look like a
#      review of an unreviewed commit. If the payload is missing, malformed, or does not satisfy the
#      schema, post NOTHING: the absence of a sha-stamped verdict is the signal.
#   2. The first line is a stable contract: `## Claude review — <40-char sha>`.
#
# Usage (from the workflow):  PR=.. SHA=.. STRUCTURED=<json> scripts/post-review-verdict.sh
# Env:
#   PR, SHA, STRUCTURED   required
#   DRY_RUN=1             print the comment body instead of posting (used by the tests)
#   REAL_GH               gh binary override (tests)

set -uo pipefail

PR="${PR:-}"
SHA="${SHA:-}"
STRUCTURED="${STRUCTURED:-}"
GH="${REAL_GH:-$(command -v gh || true)}"

warn() {
    echo "::warning title=Claude review produced no verdict::$1"
    echo "   Nothing posted for ${SHA:-<unknown>}: treat the commit as unreviewed."
    echo "   Re-trigger the review with: gh run rerun <run-id>"
    exit 0   # advisory: never red-X the PR; the missing verdict is the signal
}

[ -n "$PR" ] && [ -n "$SHA" ] || warn "PR/SHA not set."
[ -n "$STRUCTURED" ] || warn "No structured output for $SHA (crashed, cancelled, or silently no-oped)."
jq -e . >/dev/null 2>&1 <<<"$STRUCTURED" || warn "Structured output for $SHA is not valid JSON."

# Validate against the schema the model was given. `jq -r '.verdict'` on `{}` yields the STRING "null",
# which would otherwise sail through and post "Found 0 issue(s)" as a sha-stamped verdict — satisfying
# a verdict for a review that produced malformed output.
verdict=$(jq -r '.verdict // empty' <<<"$STRUCTURED")
coverage=$(jq -r '.coverage // empty' <<<"$STRUCTURED")
summary=$(jq -r '.summary // empty' <<<"$STRUCTURED")
count=$(jq -r 'if (.issue_count | type) == "number" then .issue_count else "" end' <<<"$STRUCTURED")

case "$verdict" in
    no-issues|issues) ;;
    *) warn "Structured output for $SHA has no valid 'verdict' (got: '${verdict:-<missing>}')." ;;
esac
# `coverage` is required by the schema. Without it a review that read half the diff is
# indistinguishable from one that read all of it, so a payload that omits it is refused like any other
# malformed verdict.
case "$coverage" in
    full|partial) ;;
    *) warn "Structured output for $SHA has no valid 'coverage' (got: '${coverage:-<missing>}')." ;;
esac
[ -n "$summary" ] || warn "Structured output for $SHA has no 'summary'."

# `issue_count` is required by the schema for BOTH verdicts, so a payload without a numeric one
# (missing, null, or a string such as "3") is malformed and gets no verdict, clean or not.
[ -n "$count" ] || warn "Structured output for $SHA has no numeric 'issue_count'."

# A clean verdict must count exactly zero issues. Anything else contradicts itself: refuse it, as the
# `issues` branch below refuses a count of zero. Otherwise findings could be posted under a clean headline.
if [ "$verdict" = "no-issues" ] && jq -e '.issue_count != 0' >/dev/null <<<"$STRUCTURED"; then
    warn "Structured output for $SHA says 'no-issues' but issue_count is '$count'."
fi

# Verify coverage against the real change list instead of trusting the model's
# self-report: every non-deleted changed path must appear in files_reviewed.
missing=""
if [ -n "${CHANGED_FILES:-}" ]; then
    expected=$(printf '%s\n' "$CHANGED_FILES" | awk -F'\t' 'NF && $1 !~ /^D/ {print $NF}' | sort -u)
    # Normalise to repo-relative: the Read tool works in absolute paths, and
    # a model may report ./x or $GITHUB_WORKSPACE/x for a file it did read.
    reviewed=$(jq -r '(.files_reviewed // [])[]' <<<"$STRUCTURED" \
        | sed -e "s#^${GITHUB_WORKSPACE:-/nonexistent}/##" -e 's#^\./##' | sort -u)
    missing=$(comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$reviewed"))
    if [ -n "$missing" ]; then
        coverage="partial"
    fi
fi

if [ "$verdict" = "no-issues" ] && [ "$coverage" = "partial" ]; then
    # Never let a partial read pass for a clean review: the headline says what actually happened.
    headline="Incomplete review: no issues found, but not every changed file was read."
elif [ "$verdict" = "no-issues" ]; then
    headline="No issues found. Checked for bugs and CLAUDE.md compliance."
else
    # A verdict of "issues" with no positive count is incoherent — refuse it rather than paper over it.
    [ -n "$count" ] && [ "$count" -ge 1 ] 2>/dev/null \
        || warn "Structured output for $SHA says 'issues' but issue_count is '${count:-<missing>}'."
    headline="Found ${count} issue(s) — see the inline comments."
    [ "$coverage" = "full" ] || headline="$headline Not every changed file was read."
fi

body=$(printf '## Claude review — %s\n\n%s\n\n%s\n' "$SHA" "$headline" "$summary")
if [ -n "$missing" ]; then
    n_missing=$(printf '%s\n' "$missing" | grep -c .)
    body=$(printf '%s\n\n**Not reviewed (%s changed file(s) missing from files_reviewed):**\n%s\n' \
        "$body" "$n_missing" "$(printf '%s\n' "$missing" | sed 's/^/- `/; s/$/`/')")
fi

if [ "${DRY_RUN:-0}" = "1" ]; then
    printf '%s\n' "$body"
    exit 0
fi

# `set -e` is deliberately off (the warn() paths must exit 0), so check this explicitly. Without it a
# failed post — auth, rate limit, network, gh missing from PATH — still printed "✅ posted", which would
# send whoever debugs the blocked merge looking in the wrong place. The gate blocks either way; this
# just makes the log honest. The workflow step is `continue-on-error`, so exiting 1 never red-X's the PR.
[ -n "$GH" ] || { echo "::error title=Cannot post verdict::gh not found on PATH"; exit 1; }
if ! "$GH" pr comment "$PR" --body "$body"; then
    echo "::error title=Cannot post verdict::gh pr comment failed for $SHA; the commit has no posted verdict."
    exit 1
fi
echo "✅ posted verdict ($verdict) for $SHA"
