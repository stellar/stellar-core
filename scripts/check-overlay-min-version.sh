#!/bin/sh

# This script checks OVERLAY_PROTOCOL_MIN_VERSION (in src/main/Config.cpp)
# against the ledger protocol version a network is running.
#
# Once a network is on ledger protocol P, nodes need software supporting P to
# stay in sync, so the minimum overlay version can be raised to the maximum
# overlay version (OVERLAY_PROTOCOL_VERSION) of the code that introduced P.
# It should never be higher than that, or it would disconnect nodes that the
# network still needs.
#
# Usage: check-overlay-min-version.sh [--apply] <network-protocol>
#
# Finds the commit in HEAD's history that raised CURRENT_LEDGER_PROTOCOL_VERSION
# to <network-protocol>, and compares OVERLAY_PROTOCOL_MIN_VERSION in the
# working tree against OVERLAY_PROTOCOL_VERSION at that commit. Exits with a
# non-zero status if the minimum is too high. If the minimum can be raised,
# says so, and with --apply rewrites it in the working tree.
#
# Requires the history of HEAD back to the commit that introduced
# <network-protocol>.

set -e

SRCDIR=$(realpath "$(dirname "$0")/..")
CONFIG_CPP=src/main/Config.cpp

APPLY=
if [ "$1" = "--apply" ]
then
    APPLY=1
    shift
fi

if [ $# -ne 1 ]
then
    echo "usage: $0 [--apply] <network-protocol>" >&2
    exit 2
fi

NETWORK_LEDGER=$1
case "$NETWORK_LEDGER" in
''|*[!0-9]*)
    echo "error: network protocol must be a number, got '$NETWORK_LEDGER'" >&2
    exit 2
    ;;
esac

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
MIN_EXPR='s/^[[:space:]]*OVERLAY_PROTOCOL_MIN_VERSION[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'

# Print the oldest commit in HEAD's history from which
# CURRENT_LEDGER_PROTOCOL_VERSION has been at least $NETWORK_LEDGER.
ledger_bump_commit()
{
    FOUND=
    for C in $(git log --format=%H -G 'CURRENT_LEDGER_PROTOCOL_VERSION[[:space:]]*=' HEAD -- "$CONFIG_CPP")
    do
        if [ "$(extract "$C" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")" -lt "$NETWORK_LEDGER" ]
        then
            break
        fi
        FOUND=$C
    done
    # A commit with no parent available is a shallow clone boundary, not
    # necessarily the commit that introduced the protocol version.
    if [ -z "$FOUND" ] || ! git rev-parse -q --verify "$FOUND^" >/dev/null
    then
        echo "error: could not find the commit that set CURRENT_LEDGER_PROTOCOL_VERSION to $NETWORK_LEDGER in the history of HEAD (shallow clone?)" >&2
        exit 2
    fi
    echo "$FOUND"
}

CODE_LEDGER=$(extract "" CURRENT_LEDGER_PROTOCOL_VERSION "$LEDGER_EXPR")
CODE_MIN=$(extract "" OVERLAY_PROTOCOL_MIN_VERSION "$MIN_EXPR")

if [ "$NETWORK_LEDGER" -gt "$CODE_LEDGER" ]
then
    echo "error: network is on protocol $NETWORK_LEDGER, but CURRENT_LEDGER_PROTOCOL_VERSION is $CODE_LEDGER" >&2
    exit 2
fi

BUMP_COMMIT=$(ledger_bump_commit)
TARGET_MIN=$(extract "$BUMP_COMMIT" OVERLAY_PROTOCOL_VERSION "$OVERLAY_EXPR")

echo "Network protocol $NETWORK_LEDGER was introduced in $(git log -1 --format='%h (%s)' "$BUMP_COMMIT")"
echo "with OVERLAY_PROTOCOL_VERSION $TARGET_MIN"
echo "OVERLAY_PROTOCOL_MIN_VERSION: $CODE_MIN"

if [ "$CODE_MIN" -gt "$TARGET_MIN" ]
then
    echo "error: OVERLAY_PROTOCOL_MIN_VERSION ($CODE_MIN) is higher than the overlay version" >&2
    echo "of the code that introduced protocol $NETWORK_LEDGER ($TARGET_MIN), so it would" >&2
    echo "disconnect nodes that can still follow the network." >&2
    exit 1
fi

if [ "$CODE_MIN" -eq "$TARGET_MIN" ]
then
    echo "OK"
    exit 0
fi

echo "OVERLAY_PROTOCOL_MIN_VERSION can be raised from $CODE_MIN to $TARGET_MIN"
if [ -n "$APPLY" ]
then
    sed "s/^\([[:space:]]*OVERLAY_PROTOCOL_MIN_VERSION[[:space:]]*=[[:space:]]*\)$CODE_MIN;/\1$TARGET_MIN;/" "$CONFIG_CPP" > "$CONFIG_CPP.tmp"
    mv "$CONFIG_CPP.tmp" "$CONFIG_CPP"
    if [ "$(extract "" OVERLAY_PROTOCOL_MIN_VERSION "$MIN_EXPR")" != "$TARGET_MIN" ]
    then
        echo "error: failed to update OVERLAY_PROTOCOL_MIN_VERSION in $CONFIG_CPP" >&2
        exit 2
    fi
    echo "Updated $CONFIG_CPP"
fi
