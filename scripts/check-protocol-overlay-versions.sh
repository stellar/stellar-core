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
# <head-rev> is omitted) against <base-rev>, and exits with a non-zero
# status if CURRENT_LEDGER_PROTOCOL_VERSION increased without
# OVERLAY_PROTOCOL_VERSION increasing as well.

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

LEDGER_EXPR='s/.*Config::CURRENT_LEDGER_PROTOCOL_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'
OVERLAY_EXPR='s/^[[:space:]]*OVERLAY_PROTOCOL_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'

BASE_LEDGER=$(extract "$BASE_REV" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")
BASE_OVERLAY=$(extract "$BASE_REV" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")
HEAD_LEDGER=$(extract "$HEAD_REV" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")
HEAD_OVERLAY=$(extract "$HEAD_REV" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")

echo "CURRENT_LEDGER_PROTOCOL_VERSION: $BASE_LEDGER -> $HEAD_LEDGER"
echo "OVERLAY_PROTOCOL_VERSION:        $BASE_OVERLAY -> $HEAD_OVERLAY"

if [ "$HEAD_LEDGER" -gt "$BASE_LEDGER" ] && [ "$HEAD_OVERLAY" -le "$BASE_OVERLAY" ]
then
    echo "error: CURRENT_LEDGER_PROTOCOL_VERSION was bumped from $BASE_LEDGER to $HEAD_LEDGER" >&2
    echo "without bumping OVERLAY_PROTOCOL_VERSION (still $HEAD_OVERLAY)." >&2
    echo "A ledger protocol bump must be accompanied by a bump of the maximum" >&2
    echo "overlay protocol version in $CONFIG_CPP." >&2
    exit 1
fi

echo "OK"
