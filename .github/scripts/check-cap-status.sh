#!/usr/bin/env bash
#
# Scans a PR's diff for newly added `#ifdef CAP_XXXX` / `defined(CAP_XXXX)`
# references in C/C++ source files under src/ and verifies each referenced
# CAP is in "Accepted" status on stellar/stellar-protocol@master. Posts (or
# updates) a single warning comment on the PR if any are not Accepted.
#
# Soft warning only: always exits 0 so it never blocks merges.
#
# Required env:
#   BASE_SHA   base ref SHA to diff against
#   HEAD_SHA   PR head SHA
#   PR_NUMBER  PR number for commenting
#   REPO       owner/name of the repo (for gh api comment update)
#   GH_TOKEN   token with pull-requests: write
set -euo pipefail

MARKER="<!-- cap-status-check -->"
PROTOCOL_BASE="https://raw.githubusercontent.com/stellar/stellar-protocol/master/core"
PROTOCOL_VIEW="https://github.com/stellar/stellar-protocol/blob/master/core"

# Extract CAP_NNNN identifiers from lines added by this PR in C/C++ source
# files under src/. Uses awk to track the current file from `diff --git`
# headers so we only emit matches from qualifying paths.
mapfile -t CAPS < <(
  git diff --unified=0 --diff-filter=AM "${BASE_SHA}" "${HEAD_SHA}" -- '*.h' '*.hpp' '*.cpp' \
    | awk '
        /^diff --git/ {
          # path is the "b/..." side
          file = $0
          sub(/.* b\//, "", file)
          ok = (file ~ /^src\/.*\.(h|hpp|cpp)$/)
          next
        }
        ok && /^\+/ && !/^\+\+\+/ {
          s = $0
          while (match(s, /CAP_[0-9]{4}/)) {
            print substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART + RLENGTH)
          }
        }
      ' \
    | sort -u
)

if [ "${#CAPS[@]}" -eq 0 ]; then
  echo "No CAP_XXXX references added in this PR."
  exit 0
fi

echo "Found CAP references in diff: ${CAPS[*]}"

NON_ACCEPTED=""
for cap in "${CAPS[@]}"; do
  num="${cap#CAP_}"
  url="${PROTOCOL_BASE}/cap-${num}.md"
  view_url="${PROTOCOL_VIEW}/cap-${num}.md"

  http_code=$(curl -sSL -o /tmp/cap.md -w '%{http_code}' "$url" || echo "000")
  if [ "$http_code" != "200" ]; then
    NON_ACCEPTED+="- \`${cap}\`: CAP markdown not found at [${url}](${url}) (HTTP ${http_code})"$'\n'
    continue
  fi

  # Status line lives in the leading code-fenced preamble. Grab the first
  # `Status:` line and strip the prefix + surrounding whitespace.
  status=$(grep -m1 -E '^Status:' /tmp/cap.md | sed -E 's/^Status:[[:space:]]*//; s/[[:space:]]+$//')

  if [ "$status" != "Accepted" ]; then
    NON_ACCEPTED+="- \`${cap}\`: status is **${status:-unknown}** ([cap-${num}.md](${view_url}))"$'\n'
  fi
done

if [ -z "$NON_ACCEPTED" ]; then
  echo "All referenced CAPs are Accepted."
  exit 0
fi

COMMENT=$(cat <<EOF
${MARKER}
:warning: **CAP status check**

This PR adds \`#ifdef CAP_XXXX\` / \`defined(CAP_XXXX)\` references for CAPs that are **not** in \`Accepted\` status on [stellar/stellar-protocol](https://github.com/stellar/stellar-protocol/tree/master/core):

${NON_ACCEPTED}
Only CAPs that are **Accepted** by the Core CAP committee are ready to be implemented in stellar-core. 
Please work with the CAP committee to get any non-Accepted CAPs to that status before implementing them in stellar-core. For more info, see the [CAP process](https://github.com/stellar/stellar-protocol/blob/master/core/README.md#cap-process).
EOF
)

# Find an existing marker comment to update in place (avoids spamming on each
# push). gh pr view returns issue comments; the comment id is the trailing
# integer of the URL fragment.
existing_url=$(gh pr view "$PR_NUMBER" --repo "$REPO" --json comments \
  --jq ".comments[] | select(.body | contains(\"${MARKER}\")) | .url" | head -1 || true)

if [ -n "$existing_url" ]; then
  comment_id="${existing_url##*-}"
  echo "Updating existing comment ${comment_id}"
  gh api -X PATCH "repos/${REPO}/issues/comments/${comment_id}" -f body="$COMMENT" >/dev/null
else
  echo "Posting new warning comment"
  gh pr comment "$PR_NUMBER" --repo "$REPO" --body "$COMMENT"
fi

exit 0
