#!/usr/bin/env bash
#
# Rewrites a settings file with testnet's live Soroban settings.
#
# After a protocol upgrade, testnet's settings can include new cost types,
# recalibrated cost params, and new upgradeable settings, and validators reject
# a settings upgrade that doesn't match the protocol. This reads every
# upgradeable ConfigSetting ledger entry through testnet's RPC and writes them
# to the file in ConfigSettingID order, so applying the file after a network
# reset reproduces testnet's current settings. It only ever reads from testnet,
# and refuses to run if the RPC reports a different network.
#
# Only settings a ConfigUpgradeSet can set are covered. Network-maintained
# entries, and the frozen-ledger-keys and freeze-bypass lists (changed through
# their own delta upgrades), are not, so applying the file doesn't restore them.
#
# The setting types come from stellar-xdr's ConfigSettingID enum, and the
# non-upgradeable ones are read from stellar-core's own
# SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry, so neither list is
# kept here.
#
# Usage:
#   scripts/sync-soroban-settings.sh [options]
#
# Options:
#   --file PATH       Settings file (default: soroban-settings/testnet_settings_upgrade.json)
#   --core-src PATH   stellar-core source defining isNonUpgradeableConfigSettingEntry
#                     (default: src/ledger/NetworkConfig.cpp)
#
# Prints a summary of what changed; the file is left untouched if nothing did.
#
# Requires: stellar-xdr (any release; the script checks it against testnet), curl, jq, xxd

set -euo pipefail

RPC_URL="https://soroban-testnet.stellar.org"
FILE="soroban-settings/testnet_settings_upgrade.json"
CORE_SRC="src/ledger/NetworkConfig.cpp"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --file)     FILE="$2"; shift 2 ;;
        --core-src) CORE_SRC="$2"; shift 2 ;;
        -h|--help)  awk 'NR < 3 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; exit 0 ;;
        *)          die "unknown argument: $1" ;;
    esac
done

for tool in curl jq stellar-xdr xxd; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found in PATH"
done
[ -r "$FILE" ] || die "cannot read $FILE"
[ -r "$CORE_SRC" ] || die "cannot read $CORE_SRC"

rpc()
{
    local method="$1" params="${2:-null}" resp
    resp="$(curl -sS --retry 3 -m 30 -X POST "$RPC_URL" -H 'Content-Type: application/json' \
        -d "$(jq -nc --arg m "$method" --argjson p "$params" '{jsonrpc:"2.0", id:1, method:$m, params:$p}')")" \
        || die "RPC request failed: $method"
    printf '%s' "$resp" | jq -e 'has("result")' >/dev/null \
        || die "RPC error for $method: $(printf '%s' "$resp" | jq -c '.error // .')"
    printf '%s' "$resp" | jq -c '.result'
}

NETWORK_INFO="$(rpc getNetwork)"
[ "$(printf '%s' "$NETWORK_INFO" | jq -r '.passphrase')" = "Test SDF Network ; September 2015" ] \
    || die "$RPC_URL is not testnet"
PROTOCOL="$(printf '%s' "$NETWORK_INFO" | jq -r '.protocolVersion')"

# stellar-xdr release numbers don't track protocols one-to-one (there is no
# 29.x), so instead of comparing versions, this checks the setting types it
# knows against what testnet's RPC accepts and has.
XDR_VERSION="$(stellar-xdr version | awk 'NR == 1 { print $2 }')"

# Every ConfigSettingID, in id order.
ALL_IDS="$(stellar-xdr types schema --type ConfigSettingId | jq -c '.enum')"
[ "$(printf '%s' "$ALL_IDS" | jq 'length')" -gt 0 ] \
    || die "could not list ConfigSettingIDs from stellar-xdr"

# getLedgerEntries, but returns the raw response instead of dying on an error.
get_entries_raw()
{
    curl -sS --retry 3 -m 30 -X POST "$RPC_URL" -H 'Content-Type: application/json' \
        -d "$(jq -nc --argjson k "$1" '{jsonrpc:"2.0", id:1, method:"getLedgerEntries", params:{keys:$k}}')" \
        || die "RPC request failed: getLedgerEntries"
}

# The index of the key the RPC rejected because its XDR doesn't know that
# ConfigSettingID, or nothing.
rejected_index()
{
    printf '%s' "$1" | jq -r '.error.message // ""
        | capture("cannot unmarshal key value .* at index (?<i>[0-9]+)").i // empty'
}

# If testnet has a setting newer than this stellar-xdr, it would be silently
# left out of the file, so look at the ids just past the last one stellar-xdr
# knows and fail if testnet has an entry for any. Several are checked because
# some ids (the *_delta ones) never have an entry, so a newer stored setting
# can sit behind one. The first id the RPC rejects ends the search: the RPC
# doesn't know it, so testnet can't have it or anything after it.
#
# A ConfigSetting LedgerKey is the CONFIG_SETTING (8) discriminant followed by
# the ConfigSettingID, both big-endian 32-bit.
config_key_for_id()
{
    printf '%08x%08x' 8 "$1" | xxd -r -p | base64
}
NEXT_ID="$(printf '%s' "$ALL_IDS" | jq 'length')"
for id in $(seq "$NEXT_ID" $((NEXT_ID + 3))); do
    resp="$(get_entries_raw "$(jq -nc --arg k "$(config_key_for_id "$id")" '[$k]')")"
    [ -z "$(rejected_index "$resp")" ] || break
    printf '%s' "$resp" | jq -e 'has("result")' >/dev/null \
        || die "RPC error for getLedgerEntries: $(printf '%s' "$resp" | jq -c '.error // .')"
    [ "$(printf '%s' "$resp" | jq '.result.entries // [] | length')" -eq 0 ] \
        || die "testnet has ConfigSettingID $id, which stellar-xdr $XDR_VERSION doesn't know; use a newer stellar-xdr"
done

# The ids core never sets by upgrade, read from the body of
# isNonUpgradeableConfigSettingEntry(ConfigSettingID const&), ignoring //
# comments. CONFIG_SETTING_FOO_BAR in core is foo_bar in stellar-xdr's JSON.
SKIP="$(awk '
    /SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry\(/ { sig = 1; next }
    sig && /ConfigSettingID const/ { body = 1; sig = 0; next }
    sig { sig = 0 }
    body && /^}/ { exit }
    body' "$CORE_SRC" \
    | sed 's://.*$::' \
    | { grep -o 'CONFIG_SETTING_[A-Z0-9_]*' || true; } \
    | sed 's/^CONFIG_SETTING_//' | tr 'A-Z' 'a-z' \
    | jq -Rsc 'split("\n") | map(select(length > 0))')"
[ "$(printf '%s' "$SKIP" | jq 'length')" -gt 0 ] \
    || die "found no non-upgradeable settings in isNonUpgradeableConfigSettingEntry in $CORE_SRC; has it moved?"
unknown="$(jq -nr --argjson s "$SKIP" --argjson a "$ALL_IDS" '$s - $a | join(", ")')"
[ -z "$unknown" ] \
    || die "$CORE_SRC lists settings stellar-xdr $XDR_VERSION doesn't know: $unknown"

WANT="$(jq -nc --argjson s "$SKIP" --argjson a "$ALL_IDS" '$a - $s')"

# Built in a plain loop rather than a pipeline so that a failed encode stops the
# script instead of silently dropping that setting.
keys='[]'
for id in $(printf '%s' "$WANT" | jq -r '.[]'); do
    key="$(jq -nc --arg id "$id" '{config_setting:{config_setting_id:$id}}' \
        | stellar-xdr encode --type LedgerKey)" \
        || die "stellar-xdr could not encode the LedgerKey for $id"
    keys="$(printf '%s' "$keys" | jq -c --arg k "$key" '. + [$k]')"
done

# Settings this stellar-xdr knows but testnet's RPC doesn't can't exist on
# testnet yet, so drop each one the RPC rejects and ask again.
while :; do
    resp="$(get_entries_raw "$keys")"
    bad="$(rejected_index "$resp")"
    [ -n "$bad" ] || break
    [ "$bad" -lt "$(printf '%s' "$keys" | jq 'length')" ] \
        || die "RPC rejected key $bad of $(printf '%s' "$keys" | jq 'length')"
    echo "testnet's RPC doesn't know $(printf '%s' "$WANT" | jq -r --argjson i "$bad" '.[$i]'); skipping it" >&2
    keys="$(printf '%s' "$keys" | jq -c --argjson i "$bad" 'del(.[$i])')"
    WANT="$(printf '%s' "$WANT" | jq -c --argjson i "$bad" 'del(.[$i])')"
done
printf '%s' "$resp" | jq -e 'has("result")' >/dev/null \
    || die "RPC error for getLedgerEntries: $(printf '%s' "$resp" | jq -c '.error // .')"

# Upgradeable settings with a ledger entry (the *_delta ids only exist inside
# upgrades and have none). Decoded in a plain loop, like the keys above, so a
# failed decode stops the script; each must be a setting that was asked for,
# at most once.
entries="$(printf '%s' "$resp" | jq -c '.result.entries // []')"
live='[]'
count="$(printf '%s' "$entries" | jq 'length')"
i=0
while [ "$i" -lt "$count" ]; do
    xdr="$(printf '%s' "$entries" | jq -r --argjson i "$i" '.[$i].xdr')"
    setting="$(printf '%s' "$xdr" | stellar-xdr decode --type LedgerEntryData --output json \
        | jq -c '.config_setting // empty')" \
        || die "stellar-xdr could not decode ledger entry $i returned by $RPC_URL"
    [ -n "$setting" ] || die "$RPC_URL returned a ledger entry that isn't a config setting"
    id="$(printf '%s' "$setting" | jq -r 'keys[0]')"
    printf '%s' "$WANT" | jq -e --arg id "$id" 'index($id) != null' >/dev/null \
        || die "$RPC_URL returned $id, which wasn't requested"
    printf '%s' "$live" | jq -e --arg id "$id" 'map(keys[0]) | index($id) == null' >/dev/null \
        || die "$RPC_URL returned $id more than once"
    live="$(printf '%s' "$live" | jq -c --argjson s "$setting" '. + [$s]')"
    i=$((i + 1))
done
live="$(printf '%s' "$live" | jq -c --argjson a "$ALL_IDS" 'sort_by(keys[0] as $k | $a | index($k))')"

# A setting never disappears from a network, so one the file has but testnet
# didn't return means the lookup went wrong (a partial response, or an RPC that
# doesn't know that setting). Fail rather than propose deleting it.
missing="$(jq -r --argjson live "$live" \
    '[.updated_entry[] | keys[0]] - [$live[] | keys[0]] | join(", ")' "$FILE")"
[ -z "$missing" ] || die "testnet returned no $missing, which $FILE has; refusing to remove it"

updated="$(jq --argjson live "$live" '.updated_entry = $live' "$FILE")"
# Encoding the result checks it is still a valid ConfigUpgradeSet.
printf '%s' "$updated" | stellar-xdr encode --type ConfigUpgradeSet >/dev/null \
    || die "the updated file is not a valid ConfigUpgradeSet"

echo "testnet is on protocol $PROTOCOL ($RPC_URL)"
if [ "$(jq -c . "$FILE")" = "$(printf '%s' "$updated" | jq -c .)" ]; then
    echo "$FILE already matches testnet"
    exit 0
fi

# One line per setting that was added, removed or changed. Array settings (the
# cost params) list the changed and added indices; object settings list the
# changed fields.
jq -rn --slurpfile old "$FILE" --argjson new "$updated" '
    def bykey: map(to_entries[0]) | from_entries;
    ($old[0].updated_entry | bykey) as $o | ($new.updated_entry | bykey) as $n
    | (($o | keys) + ($n | keys) | unique)[] as $k
    | if $o[$k] == null then "\($k): added"
      elif $n[$k] == null then "\($k): removed"
      elif $o[$k] == $n[$k] then empty
      elif ($n[$k] | type) == "array" then
        [range(0; [($o[$k] | length), ($n[$k] | length)] | min) | select($o[$k][.] != $n[$k][.])] as $c
        | "\($k): \($o[$k] | length) -> \($n[$k] | length) entries"
          + (if ($c | length) > 0 then ", changed \($c | map(tostring) | join(", "))" else "" end)
          + (if ($n[$k] | length) > ($o[$k] | length) then ", added \($o[$k] | length)-\(($n[$k] | length) - 1)" else "" end)
      elif ($n[$k] | type) == "object" then
        "\($k): changed \([($n[$k] | keys[]) as $f | select($o[$k][$f] != $n[$k][$f]) | $f] | join(", "))"
      else "\($k): \($o[$k]) -> \($n[$k])"
      end'

# Keep the file's existing trailing-newline convention so the diff is only the values.
if [ -n "$(tail -c 1 "$FILE")" ]; then
    printf '%s' "$updated" > "$FILE"
else
    printf '%s\n' "$updated" > "$FILE"
fi
echo "updated $FILE"
