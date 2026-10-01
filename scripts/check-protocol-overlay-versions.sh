#!/bin/sh

# This script checks that a change that bumps the current ledger protocol
# version (Config::CURRENT_LEDGER_PROTOCOL_VERSION in src/main/Config.cpp)
# also bumps the maximum supported overlay protocol version
# (OVERLAY_PROTOCOL_VERSION in the same file), so that peers can tell from
# the overlay handshake whether a node runs software that supports the new
# ledger protocol.
#
# Usage: check-protocol-overlay-versions.sh <base-rev> [<head-rev>]
#
# Compares src/main/Config.cpp at <head-rev> (or the working tree copy if
# <head-rev> is omitted) against <base-rev>. If CURRENT_LEDGER_PROTOCOL_VERSION
# increased, exits with a non-zero status unless OVERLAY_PROTOCOL_VERSION is
# higher than it was when <base-rev>'s ledger protocol version was introduced.
# Comparing against that commit rather than <base-rev> means a change doesn't
# need to bump the overlay version again if it was already bumped since the
# previous protocol bump. It also doesn't depend on release tags, which may not
# be public yet when the next protocol bump lands.
#
# Requires the history of <base-rev> back to its last ledger protocol bump.

set -e

SRCDIR=$(realpath $(dirname $0)/..)
CONFIG_CPP=src/main/Config.cpp

if [ $# -lt 1 ] || [ $# -gt 2 ]
then
    echo "usage: $0 <base-rev> [<head-rev>]" >&2
    exit 2
fi

BASE_REV=$1
HEAD_REV=${2:-}

cd "$SRCDIR"

# Print the contents of src/main/Config.cpp at revision $1, or the working
# tree copy if $1 is empty.
config_at()
{
    if [ -n "$1" ]
    then
        git show "$1:$CONFIG_CPP"
    else
        cat "$CONFIG_CPP"
    fi
}

# extract <rev> <name> <sed-expr>: print the numeric value assigned to
# <name> in Config.cpp at <rev>, failing unless exactly one assignment is
# found.
extract()
{
    if ! CONTENTS=$(config_at "$1")
    then
        echo "error: failed to read $CONFIG_CPP at ${1:-working tree}" >&2
        exit 2
    fi
    VAL=$(printf '%s\n' "$CONTENTS" | sed -n "$3")
    case "$VAL" in
    ''|*[!0-9]*)
        echo "error: expected exactly one numeric assignment to $2 in $CONFIG_CPP at ${1:-working tree}, got '$VAL'" >&2
        echo "(if the definition of $2 changed shape, update $0 to match)" >&2
        exit 2
        ;;
    esac
    echo "$VAL"
}

# Print the oldest commit in <base-rev>'s history from which
# CURRENT_LEDGER_PROTOCOL_VERSION has had the value it has at <base-rev>.
ledger_bump_commit()
{
    FOUND=
    for C in $(git log --format=%H -G 'CURRENT_LEDGER_PROTOCOL_VERSION[[:space:]]*=' "$BASE_REV" -- "$CONFIG_CPP")
    do
        if [ "$(extract "$C" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")" != "$BASE_LEDGER" ]
        then
            break
        fi
        FOUND=$C
    done
    # A commit with no parent available is a shallow clone boundary, not
    # necessarily the commit that introduced the protocol version.
    if [ -z "$FOUND" ] || ! git rev-parse -q --verify "$FOUND^" >/dev/null
    then
        echo "error: could not find the commit that set CURRENT_LEDGER_PROTOCOL_VERSION to $BASE_LEDGER in the history of $BASE_REV (shallow clone?)" >&2
        exit 2
    fi
    echo "$FOUND"
}

LEDGER_EXPR='s/.*Config::CURRENT_LEDGER_PROTOCOL_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'
OVERLAY_EXPR='s/^[[:space:]]*OVERLAY_PROTOCOL_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'

BASE_LEDGER=$(extract "$BASE_REV" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")
BASE_OVERLAY=$(extract "$BASE_REV" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")
HEAD_LEDGER=$(extract "$HEAD_REV" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")
HEAD_OVERLAY=$(extract "$HEAD_REV" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")

echo "CURRENT_LEDGER_PROTOCOL_VERSION: $BASE_LEDGER -> $HEAD_LEDGER"
echo "OVERLAY_PROTOCOL_VERSION:        $BASE_OVERLAY -> $HEAD_OVERLAY"

if [ "$HEAD_LEDGER" -gt "$BASE_LEDGER" ]
then
    BUMP_COMMIT=$(ledger_bump_commit)
    BUMP_OVERLAY=$(extract "$BUMP_COMMIT" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")
    echo "Protocol $BASE_LEDGER was introduced in $(git log -1 --format='%h (%s)' "$BUMP_COMMIT")"
    echo "with OVERLAY_PROTOCOL_VERSION $BUMP_OVERLAY"
    if [ "$HEAD_OVERLAY" -le "$BUMP_OVERLAY" ]
    then
        echo "error: CURRENT_LEDGER_PROTOCOL_VERSION was bumped from $BASE_LEDGER to $HEAD_LEDGER" >&2
        echo "but OVERLAY_PROTOCOL_VERSION ($HEAD_OVERLAY) has not been bumped since protocol" >&2
        echo "$BASE_LEDGER was introduced. A ledger protocol bump must be accompanied by a" >&2
        echo "bump of the maximum overlay protocol version in $CONFIG_CPP." >&2
        exit 1
    fi
fi

echo "OK"
