#!/bin/sh

# This script checks that a change that bumps the ledger protocol version
# (MAX_SUPPORTED_PROTOCOL_VERSION in src/util/ProtocolVersion.h) also bumps
# the maximum supported overlay protocol version
# (OVERLAY_PROTOCOL_VERSION in src/main/Config.cpp), so that peers can tell
# from the overlay handshake whether a node runs software that supports the
# new ledger protocol.
#
# Usage: check-protocol-overlay-versions.sh <base-rev> [<head-rev>]
#
# Compares <head-rev> (or the working tree if <head-rev> is omitted) against
# <base-rev>. Exits with a non-zero status if OVERLAY_PROTOCOL_VERSION was
# lowered, or if the ledger protocol version increased without
# OVERLAY_PROTOCOL_VERSION increasing in the same change. An overlay bump that
# landed separately (before or after an earlier protocol bump) doesn't count,
# since it can't be told apart from one that belongs to the previous protocol.

set -e

SRCDIR=$(realpath "$(dirname "$0")/..")
CONFIG_CPP=src/main/Config.cpp
PROTOCOL_H=src/util/ProtocolVersion.h

if [ $# -lt 1 ] || [ $# -gt 2 ]
then
    echo "usage: $0 <base-rev> [<head-rev>]" >&2
    exit 2
fi

BASE_REV=$1
HEAD_REV=${2:-}

cd "$SRCDIR"

# file_at <rev> <file>: print the contents of <file> at <rev>, or the
# working tree copy if <rev> is empty.
file_at()
{
    if [ -n "$1" ]
    then
        git show "$1:$2"
    else
        cat "$2"
    fi
}

# extract <rev> <file> <name> <sed-expr>: print the numeric value assigned
# to <name> in <file> at <rev>, failing unless exactly one assignment is
# found.
extract()
{
    if ! CONTENTS=$(file_at "$1" "$2")
    then
        echo "error: failed to read $2 at ${1:-working tree}" >&2
        exit 2
    fi
    VAL=$(printf '%s\n' "$CONTENTS" | sed -n "$4")
    case "$VAL" in
    ''|*[!0-9]*)
        echo "error: expected exactly one numeric assignment to $3 in $2 at ${1:-working tree}, got '$VAL'" >&2
        echo "(if the definition of $3 changed shape, update $0 to match)" >&2
        exit 2
        ;;
    esac
    echo "$VAL"
}

LEDGER_EXPR='s/.*MAX_SUPPORTED_PROTOCOL_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'
OVERLAY_EXPR='s/^[[:space:]]*OVERLAY_PROTOCOL_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'

# The literal assigned to MAX_SUPPORTED_PROTOCOL_VERSION, i.e. the ledger
# protocol version without the vnext +1.
BASE_LEDGER=$(extract "$BASE_REV" "$PROTOCOL_H" MAX_SUPPORTED_PROTOCOL_VERSION "$LEDGER_EXPR")
BASE_OVERLAY=$(extract "$BASE_REV" "$CONFIG_CPP" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")
HEAD_LEDGER=$(extract "$HEAD_REV" "$PROTOCOL_H" MAX_SUPPORTED_PROTOCOL_VERSION "$LEDGER_EXPR")
HEAD_OVERLAY=$(extract "$HEAD_REV" "$CONFIG_CPP" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")

echo "Ledger protocol version:         $BASE_LEDGER -> $HEAD_LEDGER"
echo "OVERLAY_PROTOCOL_VERSION:        $BASE_OVERLAY -> $HEAD_OVERLAY"

if [ "$HEAD_OVERLAY" -lt "$BASE_OVERLAY" ]
then
    echo "error: OVERLAY_PROTOCOL_VERSION was lowered from $BASE_OVERLAY to $HEAD_OVERLAY" >&2
    exit 1
fi

if [ "$HEAD_LEDGER" -gt "$BASE_LEDGER" ] && [ "$HEAD_OVERLAY" -le "$BASE_OVERLAY" ]
then
    echo "error: the ledger protocol version was bumped from $BASE_LEDGER to $HEAD_LEDGER" >&2
    echo "but OVERLAY_PROTOCOL_VERSION was not bumped. A ledger protocol bump must be" >&2
    echo "accompanied by a bump of the maximum overlay protocol version in $CONFIG_CPP." >&2
    exit 1
fi

echo "OK"
