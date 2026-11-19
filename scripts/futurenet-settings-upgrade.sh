#!/usr/bin/env bash
#
# Soroban settings upgrade for futurenet, meant to run as part of the futurenet
# reset, from anywhere that can reach the validators' HTTP admin ports.
#
# For each settings file, in order, it:
#   1. fetches the file from stellar-core (or reads a local path),
#   2. fits the cost params to futurenet: futurenet often runs a protocol ahead
#      of testnet, with cost types the testnet-derived file doesn't have yet, so
#      the network's current values are appended for those,
#   3. submits the four settings-upgrade transactions built by
#      `stellar-core get-settings-upgrade-txs` through the RPC,
#   4. checks the proposed set on every validator (dumpproposedsettings),
#   5. checks nothing else is armed, then arms it on every validator,
#   6. waits until a ledger applies it, by finding its ConfigUpgradeSetKey in
#      the upgrades of a closed ledger's header (no settings are decoded, so
#      this works whatever futurenet's protocol),
# before starting the next file, which may depend on limits the previous one
# raised.
#
# Usage:
#   futurenet-settings-upgrade.sh <settings> [more ...] [options]
#   futurenet-settings-upgrade.sh --schedule-only <ConfigUpgradeSetKey> [options]
#
# A <settings> argument is a path to a local file if one exists there, and
# otherwise the name of a file in stellar-core's soroban-settings/ directory at
# --settings-ref (default master), e.g. the reset's two phases:
#   futurenet-settings-upgrade.sh --node-urls "<validator admin URLs>" \
#       --secret-file <file> --yes testnet_settings_enable_upgrades testnet_settings_upgrade
#
# The source account is --secret-file (during a reset, the network's root
# account), else --secret, else a fresh friendbot-funded account.
#
# Requires: stellar-core, stellar-xdr, curl, jq, xxd, base64.

set -euo pipefail

RPC_URL="https://rpc-futurenet.stellar.org"
PASSPHRASE="Test SDF Future Network ; October 2022"
FRIENDBOT_URL="https://friendbot-futurenet.stellar.org"

# The validators' admin URLs (--node-urls, required).
NODE_URLS=""

SETTINGS_REPO="stellar/stellar-core"
SETTINGS_DIR="soroban-settings"
SETTINGS_REF="master"

CORE_BIN=""
RESOURCE_FEE="100"
UPGRADE_IN="90s"
SCHEDULE_ONLY_KEY=""
ASSUME_YES="false"
DRY_RUN="false"
NO_SCHEDULE="false"

HTTP_TIMEOUT="30"
TX_WAIT_SECONDS="120"
ACCOUNT_WAIT_SECONDS="60"
# How long after the scheduled upgrade time to keep looking for the ledger
# that applied it.
APPLY_WAIT_SECONDS="600"
POLL_INTERVAL="2"

SECRET_KEY=""
PUBLIC_KEY=""
SETTINGS_FILES=""
RESOLVED_FILES=""
TMP_DIR=""

usage()
{
    # Print the header comment block, minus the shebang.
    awk 'NR < 3 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
    cat <<'EOF'

Options:
  --secret-file PATH      Read the source account's secret seed from PATH
  --secret SK             Use this source account (visible in the process list;
                          prefer --secret-file)
  --node-urls "U1 U2 .."  Every futurenet validator's admin URL, e.g.
                          "http://host-a:11626 http://host-b:11626" (required)
  --settings-ref REF      stellar-core branch, tag or commit to fetch settings
                          files from (default: master); resolved to a commit
                          once, so every phase uses the same one
  --core-bin PATH         stellar-core binary (default: stellar-core in PATH)
  --resource-fee N        Additional resource fee added to every tx (default: 100)
  --upgrade-in DURATION   Schedule each phase this far out, as 90s / 5m / a bare
                          number of minutes (default: 90s)
  --apply-timeout SECS    How long after the upgrade time to wait for a ledger
                          to apply it (default: 600)
  --schedule-only KEY     Skip submission; arm an already-submitted
                          ConfigUpgradeSetKey and wait for it to apply
  --no-schedule           Submit one settings file and check it on the
                          validators, but arm nothing
  --dry-run               Build the transactions and run the read-only checks;
                          submit and arm nothing
  --rpc URL               RPC endpoint (default: https://rpc-futurenet.stellar.org)
  --passphrase STR        Network passphrase (default: futurenet's)
  --yes                   Don't prompt before arming
  -h, --help              This message
EOF
}

log()  { printf '%s\n' "$*" >&2; }
step() { printf '\n=== %s ===\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

confirm()
{
    [ "$ASSUME_YES" = "true" ] && return 0
    local reply
    printf '%s [y/N] ' "$1" >&2
    read -r reply </dev/tty || die "no tty available for confirmation; re-run with --yes"
    case "$reply" in
        y|Y|yes|YES) return 0 ;;
        *) die "aborted" ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --secret-file)    [ -r "$2" ] || die "cannot read secret file: $2"
                          SECRET_KEY="$(tr -d '[:space:]' < "$2")"
                          [ -n "$SECRET_KEY" ] || die "secret file is empty: $2"
                          shift 2 ;;
        --secret)         SECRET_KEY="$2"; shift 2 ;;
        --node-urls)      NODE_URLS="$2"; shift 2 ;;
        --settings-ref)   SETTINGS_REF="$2"; shift 2 ;;
        --core-bin)       CORE_BIN="$2"; shift 2 ;;
        --resource-fee)   RESOURCE_FEE="$2"; shift 2 ;;
        --upgrade-in)     UPGRADE_IN="$2"; shift 2 ;;
        --apply-timeout)  APPLY_WAIT_SECONDS="$2"; shift 2 ;;
        --schedule-only)  SCHEDULE_ONLY_KEY="$2"; shift 2 ;;
        --no-schedule)    NO_SCHEDULE="true"; shift ;;
        --dry-run)        DRY_RUN="true"; shift ;;
        --rpc)            RPC_URL="$2"; shift 2 ;;
        --passphrase)     PASSPHRASE="$2"; shift 2 ;;
        --yes|-y)         ASSUME_YES="true"; shift ;;
        -h|--help)        usage; exit 0 ;;
        -*)               die "unknown option: $1" ;;
        *)
            case "$1" in *[[:space:]]*) die "settings paths must not contain spaces: $1" ;; esac
            SETTINGS_FILES="$SETTINGS_FILES $1"
            shift ;;
    esac
done

# ------------------------------------------------------------------ helpers --

rpc()
{
    local method="$1" params="${2:-null}" body resp
    body="$(jq -nc --arg m "$method" --argjson p "$params" \
        '{jsonrpc:"2.0", id:1, method:$m, params:$p}')"
    resp="$(curl -sS -m "$HTTP_TIMEOUT" -X POST "$RPC_URL" \
        -H 'Content-Type: application/json' -d "$body")" \
        || die "RPC request failed: $method"
    [ -n "$resp" ] || die "empty RPC response for $method"
    if printf '%s' "$resp" | jq -e 'has("error")' >/dev/null; then
        die "RPC error for $method: $(printf '%s' "$resp" | jq -c '.error')"
    fi
    printf '%s' "$resp" | jq -c '.result'
}

decode_xdr()
{
    # decode_xdr <type> <base64>; best effort, for error messages only
    printf '%s' "$2" | stellar-xdr decode --type "$1" --output json 2>/dev/null || printf '%s' "$2"
}

node_http()
{
    # node_http <node url> <path> [curl args...]
    local url="$1" path="$2"; shift 2
    curl -sS -m "$HTTP_TIMEOUT" -G "$url/$path" "$@"
}

latest_ledger()
{
    rpc getLatestLedger | jq -r '.sequence'
}

to_seconds()
{
    # Accepts "90s", "10m", or a bare number of minutes.
    case "$1" in
        *s) printf '%s\n' "${1%s}" ;;
        *m) printf '%s\n' "$(( ${1%m} * 60 ))" ;;
        *)  printf '%s\n' "$(( $1 * 60 ))" ;;
    esac
}

future_time()
{
    local secs
    secs="$(to_seconds "$1")"
    if date -u -v+"${secs}"S +%Y-%m-%dT%H:%M:%SZ >/dev/null 2>&1; then
        date -u -v+"${secs}"S +%Y-%m-%dT%H:%M:%SZ            # BSD/macOS
    else
        date -u -d "+${secs} seconds" +%Y-%m-%dT%H:%M:%SZ    # GNU
    fi
}

# --------------------------------------------------------------- settings --

cleanup()
{
    [ -n "$TMP_DIR" ] && rm -rf "$TMP_DIR" 2>/dev/null || true
}

resolve_settings_ref()
{
    # Pin SETTINGS_REF to a commit, so every phase is fetched from the same one
    # and the run says exactly which settings it applied.
    local sha
    sha="$(curl -fsS -m "$HTTP_TIMEOUT" -H 'Accept: application/vnd.github+json' \
        "https://api.github.com/repos/$SETTINGS_REPO/commits/$SETTINGS_REF" | jq -r '.sha // empty')" \
        || die "could not resolve $SETTINGS_REPO@$SETTINGS_REF to a commit"
    [ -n "$sha" ] || die "could not resolve $SETTINGS_REPO@$SETTINGS_REF to a commit"
    log "settings:     $SETTINGS_REPO@$SETTINGS_REF = $sha"
    SETTINGS_REF="$sha"
}

resolve_settings()
{
    # Echoes a local path for $1, fetching it from stellar-core if it is not
    # already a local file. TMP_DIR must already exist (see the preflight).
    local arg="$1" name dest url
    if [ -e "$arg" ]; then
        [ -r "$arg" ] || die "cannot read settings file: $arg"
        log "  $arg (local)"
        printf '%s' "$arg"
        return 0
    fi
    # Only bare names are looked up remotely; anything path-shaped is a typo.
    case "$arg" in
        */*|.*) die "no such settings file: $arg" ;;
    esac
    name="${arg%.json}.json"
    dest="$TMP_DIR/$name"
    if [ ! -e "$dest" ]; then
        url="https://raw.githubusercontent.com/$SETTINGS_REPO/$SETTINGS_REF/$SETTINGS_DIR/$name"
        curl -fsSL -m "$HTTP_TIMEOUT" -o "$dest" "$url" \
            || die "could not fetch $SETTINGS_DIR/$name at $SETTINGS_REF ($url)"
    fi
    log "  $name ($SETTINGS_REPO@${SETTINGS_REF:0:12})"
    printf '%s' "$dest"
}

# --------------------------------------------------------------- validators --

check_nodes()
{
    # Every validator must answer, be synced, and agree on the protocol.
    local url info state version versions=""
    step "Validators"
    for url in $NODE_URLS; do
        info="$(node_http "$url" info)" || die "cannot reach $url"
        state="$(printf '%s' "$info" | jq -r '.info.state // empty')"
        version="$(printf '%s' "$info" | jq -r '.info.ledger.version // empty')"
        log "  $url: $state, protocol $version, $(printf '%s' "$info" | jq -r '.info.build // "?"')"
        [ "$state" = "Synced!" ] || die "$url is not synced ($state)"
        versions="$versions $version"
    done
    [ "$(printf '%s\n' $versions | sort -u | wc -l | tr -d ' ')" -eq 1 ] \
        || die "validators disagree on the protocol version:$versions"
}

check_nothing_armed()
{
    # Arming replaces a validator's whole set of scheduled upgrades, so refuse
    # if anything else (a protocol upgrade, say) is already scheduled. `time`
    # and `upgradeversion` are always present; every upgrade field is
    # {"nullopt": true} when unset.
    local url armed
    for url in $NODE_URLS; do
        armed="$(node_http "$url" upgrades --data-urlencode mode=get \
            | jq -r '[to_entries[] | select(.key != "time" and .key != "upgradeversion")
                      | select((.value | type) != "object" or .value.nullopt != true) | .key]
                     | join(", ")')" \
            || die "could not read the scheduled upgrades on $url"
        [ -z "$armed" ] || die "$url already has upgrades scheduled ($armed); clear them or wait for them before arming"
    done
    log "  nothing else is scheduled on any validator"
}

verify_on_nodes()
{
    # Every validator must accept the proposed set. dumpproposedsettings only
    # prints the settings if the set passes the same validity check validators
    # apply to an armed upgrade; otherwise it says it is missing or invalid.
    local key="$1" url out first=""
    step "Checking the proposed settings on the validators"
    for url in $NODE_URLS; do
        out="$(node_http "$url" dumpproposedsettings --data-urlencode "blob=$key")" \
            || die "dumpproposedsettings failed on $url"
        case "$out" in
            *ConfigSettingsEntries*) ;;
            *) die "$url does not accept the upgrade set $key: $out" ;;
        esac
        log "  $url: valid ($(printf '%s' "$out" | jq -r '[.ConfigSettingsEntries[].configSettingID] | join(", ")' 2>/dev/null))"
        if [ -z "$first" ]; then
            first="$out"
        elif [ "$out" != "$first" ]; then
            die "$url sees a different upgrade set than the first validator"
        fi
    done
}

clear_nodes()
{
    # Best effort, after a partial arming: nothing else was scheduled (checked
    # before arming), so clearing only removes what this run set.
    local url
    for url in "$@"; do
        node_http "$url" upgrades --data-urlencode mode=clear >/dev/null 2>&1 \
            && log "  cleared $url" || log "  could not clear $url; clear it by hand (upgrades?mode=clear)"
    done
}

arm_on_nodes()
{
    # Sets ARMED_AT. Arms every validator or, on any failure, clears the ones
    # already armed, so a partial arming can't linger.
    local key="$1" url armed_urls="" got reply
    step "Arming the upgrade on the validators"
    check_nothing_armed
    confirm "Arm$(printf ' %s' $NODE_URLS) for a settings upgrade $UPGRADE_IN from now?"
    # Resolved after the prompt, so time spent answering it can't push the
    # upgrade time into the past.
    ARMED_AT="$(future_time "$UPGRADE_IN")"
    log "  upgrade time: $ARMED_AT (UTC)"
    for url in $NODE_URLS; do
        # upgradetime goes in the path as-is (as it always has for these
        # endpoints); the key is base64, so it is URL-encoded. Core answers
        # HTTP 200 even when it refuses (e.g. "Error setting
        # configUpgradeSet"), with the reason in the body, so log the body.
        if ! reply="$(node_http "$url" "upgrades?mode=set&upgradetime=$ARMED_AT" \
                --data-urlencode "configupgradesetkey=$key")"; then
            log "failed to reach $url; clearing the validators armed so far"
            clear_nodes $armed_urls
            die "arming failed on $url"
        fi
        [ -z "$reply" ] || log "  $url replied: $reply"
        armed_urls="$armed_urls $url"
        # Confirm it took. Once a config upgrade is armed, mode=get moves its
        # key from the top-level "configupgradesetkey" to
        # "configupgradeinfo": {"configupgradesetkey": {"nullopt": false,
        # "data": "<base64 key>"}, "configupgradeset": {...}}.
        got="$(node_http "$url" upgrades --data-urlencode mode=get \
            | jq -r '.configupgradeinfo.configupgradesetkey.data // empty')"
        if [ "$got" != "$key" ]; then
            log "$url did not record the upgrade (scheduled key: ${got:-none}); clearing the validators armed so far"
            clear_nodes $armed_urls
            die "arming did not take on $url"
        fi
        log "  armed $url"
    done
}

wait_for_ledger_with_upgrade()
{
    # wait_for_ledger_with_upgrade <ConfigUpgradeSetKey> <first ledger to check>
    #
    # An armed upgrade takes effect in the ledger whose header lists it among
    # its upgrades, and only an upgrade the network agreed to and applied gets
    # there. A config upgrade is listed as the LEDGER_UPGRADE_CONFIG tag (6)
    # followed by the 64-byte ConfigUpgradeSetKey, so finding those bytes in a
    # closed ledger's header proves it applied, without decoding any settings.
    local key="$1" cursor="$2" needle deadline latest page seq header
    needle="00000006$(printf '%s' "$key" | base64 -d | xxd -p | tr -d '\n')"
    [ "${#needle}" -eq 136 ] || die "internal: $key is not a 64-byte ConfigUpgradeSetKey"
    deadline=$(( $(date -u +%s) + $(to_seconds "$UPGRADE_IN") + APPLY_WAIT_SECONDS ))
    step "Waiting for a ledger to apply it (from ledger $cursor)"
    while [ "$(date -u +%s)" -lt "$deadline" ]; do
        latest="$(latest_ledger)"
        if [ "$cursor" -le "$latest" ]; then
            page="$(rpc getLedgers "$(jq -nc --argjson s "$cursor" '{startLedger:$s, pagination:{limit:100}}')")"
            while read -r seq header; do
                [ -n "$seq" ] || continue
                if printf '%s' "$header" | base64 -d | xxd -p | tr -d '\n' | grep -q "$needle"; then
                    log "  applied in ledger $seq"
                    return 0
                fi
                cursor=$((seq + 1))
            done < <(printf '%s' "$page" | jq -r '.ledgers[]? | "\(.sequence) \(.headerXdr)"')
        fi
        sleep "$POLL_INTERVAL"
    done
    die "no ledger up to $(latest_ledger) applied $key; it may have been dropped (expired) or not nominated. Config upgrade scheduled now: $(node_http "$(printf '%s\n' $NODE_URLS | head -1)" upgrades --data-urlencode mode=get | jq -c '.configupgradeinfo.configupgradesetkey.data // "none"')"
}

# ------------------------------------------------------------------ account --

account_entry_xdr()
{
    # Echoes the base64 AccountEntry, or nothing if the account does not exist.
    local pk="$1" key
    key="$(jq -nc --arg a "$pk" '{account:{account_id:$a}}' \
        | stellar-xdr encode --type LedgerKey)" || die "could not encode the account key for $pk"
    rpc getLedgerEntries "$(jq -nc --arg k "$key" '{keys:[$k]}')" \
        | jq -r '.entries[0].xdr // empty'
}

account_seq_num()
{
    local pk="$1" entry
    entry="$(account_entry_xdr "$pk")"
    [ -n "$entry" ] || die "account $pk not found on $RPC_URL"
    printf '%s' "$entry" \
        | stellar-xdr decode --type LedgerEntryData --output json \
        | jq -r '.account.seq_num'
}

create_and_fund_account()
{
    # Sets SECRET_KEY / PUBLIC_KEY to a fresh friendbot-funded account. The
    # seed is never printed: it's throwaway, and logs may be shared.
    local seed_out http waited=0
    seed_out="$("$CORE_BIN" gen-seed </dev/null 2>/dev/null)" || die "stellar-core gen-seed failed"
    SECRET_KEY="$(printf '%s\n' "$seed_out" | awk '/^Secret seed:/ { print $NF }')"
    PUBLIC_KEY="$(printf '%s\n' "$seed_out" | awk '/^Public:/ { print $NF }')"
    [ -n "$SECRET_KEY" ] && [ -n "$PUBLIC_KEY" ] || die "could not parse stellar-core gen-seed output"
    log "generated source account: $PUBLIC_KEY"
    http="$(curl -sS -m 60 -o /dev/null -w '%{http_code}' "$FRIENDBOT_URL/?addr=$PUBLIC_KEY")" \
        || die "friendbot request to $FRIENDBOT_URL failed"
    [ "$http" = "200" ] || die "friendbot returned HTTP $http for $PUBLIC_KEY"
    while [ "$waited" -lt "$ACCOUNT_WAIT_SECONDS" ]; do
        if [ -n "$(account_entry_xdr "$PUBLIC_KEY")" ]; then
            log "  funded"
            return 0
        fi
        sleep "$POLL_INTERVAL"
        waited=$((waited + POLL_INTERVAL))
    done
    die "account $PUBLIC_KEY did not appear on $RPC_URL within ${ACCOUNT_WAIT_SECONDS}s of funding"
}

setup_account()
{
    step "Source account"
    if [ -n "$SECRET_KEY" ]; then
        PUBLIC_KEY="$("$CORE_BIN" convert-id "$SECRET_KEY" </dev/null \
            | awk '/^PublicKey:/{f=1; next} f && /strKey:/{print $NF; exit}')"
        [ -n "$PUBLIC_KEY" ] || die "could not derive a public key from the secret"
        log "using account: $PUBLIC_KEY"
    else
        create_and_fund_account
    fi
}

# ------------------------------------------------------------ transactions --

wait_for_tx()
{
    # wait_for_tx <label> <hex hash>
    local label="$1" hash="$2" waited=0 res status
    while [ "$waited" -lt "$TX_WAIT_SECONDS" ]; do
        res="$(rpc getTransaction "$(jq -nc --arg h "$hash" '{hash:$h}')")"
        status="$(printf '%s' "$res" | jq -r '.status')"
        case "$status" in
            SUCCESS)
                log "  $label: SUCCESS (ledger $(printf '%s' "$res" | jq -r '.ledger'))"
                return 0 ;;
            FAILED)
                log "  result: $(decode_xdr TransactionResult "$(printf '%s' "$res" | jq -r '.resultXdr // empty')")"
                die "$label failed on-chain; re-run the script (it reads the new sequence number)" ;;
            NOT_FOUND)
                sleep "$POLL_INTERVAL"
                waited=$((waited + POLL_INTERVAL)) ;;
            *)
                die "unexpected getTransaction status for $label: $status" ;;
        esac
    done
    die "$label ($hash) not applied within ${TX_WAIT_SECONDS}s"
}

submit_tx()
{
    # submit_tx <label> <base64 envelope> <expected hex hash>
    local label="$1" env="$2" hash="$3" res status attempt=0
    log "$label: submitting (hash $hash)"
    while :; do
        res="$(rpc sendTransaction "$(jq -nc --arg t "$env" '{transaction:$t}')")"
        status="$(printf '%s' "$res" | jq -r '.status')"
        case "$status" in
            PENDING|DUPLICATE)
                break ;;
            TRY_AGAIN_LATER)
                attempt=$((attempt + 1))
                [ "$attempt" -ge 5 ] && die "$label: RPC kept returning TRY_AGAIN_LATER"
                log "  $label: TRY_AGAIN_LATER, retrying"
                sleep 5 ;;
            ERROR)
                log "  error: $(decode_xdr TransactionResult "$(printf '%s' "$res" | jq -r '.errorResultXdr // empty')")"
                die "$label rejected by RPC" ;;
            *)
                die "$label: unexpected sendTransaction status: $status" ;;
        esac
    done
    wait_for_tx "$label" "$hash"
}

# Set by generate_and_submit for the phase in flight.
PHASE_KEY=""
PHASE_JSON=""

fit_cost_params_to_network()
{
    # Resizes each cost-params list in PHASE_JSON (a ConfigUpgradeSet as JSON)
    # to futurenet's number of cost types, in place (a $(...) call would lose
    # the log of what changed). When futurenet has more cost types than the
    # file, the file's values are kept for the ones it has and the network's
    # current values (the ones its protocol upgrade created) are appended for
    # the rest. More entries in the file than on the network is a mistake, so
    # that fails. Cost params are plain lists of {const_term, linear_term}, so
    # reading futurenet's works whatever protocol it's on.
    local json="$PHASE_JSON" id key entry live have want
    for id in contract_cost_params_cpu_instructions contract_cost_params_memory_bytes; do
        have="$(printf '%s' "$json" | jq --arg id "$id" \
            '[.updated_entry[] | select(has($id)) | .[$id] | length] | first // empty')"
        [ -n "$have" ] || continue
        key="$(jq -nc --arg id "$id" '{config_setting:{config_setting_id:$id}}' \
            | stellar-xdr encode --type LedgerKey)" || die "could not encode the LedgerKey for $id"
        entry="$(rpc getLedgerEntries "$(jq -nc --arg k "$key" '{keys:[$k]}')" \
            | jq -r '.entries[0].xdr // empty')"
        [ -n "$entry" ] || die "$RPC_URL returned no $id entry"
        live="$(printf '%s' "$entry" \
            | stellar-xdr decode --type LedgerEntryData --output json \
            | jq -c --arg id "$id" '.config_setting[$id]')" \
            || die "could not decode futurenet's $id"
        want="$(printf '%s' "$live" | jq 'length')"
        if [ "$have" -gt "$want" ]; then
            die "the settings have $have $id entries but futurenet has only $want cost types"
        elif [ "$have" -lt "$want" ]; then
            log "futurenet has $want $id cost types, the settings have $have;"
            log "  appending $have-$((want - 1)) from futurenet's current values"
            json="$(printf '%s' "$json" | jq -c --arg id "$id" --argjson live "$live" --argjson have "$have" \
                '.updated_entry |= map(if has($id) then .[$id] += $live[$have:] else . end)')"
        fi
    done
    PHASE_JSON="$json"
}

generate_and_submit()
{
    local file="$1" label="$2" xdr seq output
    step "$label: preparing $file"

    PHASE_JSON="$(stellar-xdr encode --type ConfigUpgradeSet "$file" \
        | stellar-xdr decode --type ConfigUpgradeSet --output json)" \
        || die "$file is not a valid ConfigUpgradeSet"
    fit_cost_params_to_network
    xdr="$(printf '%s' "$PHASE_JSON" | stellar-xdr encode --type ConfigUpgradeSet)" \
        || die "the settings from $file don't encode after fitting them to futurenet"

    seq="$(account_seq_num "$PUBLIC_KEY")"
    log "sequence number: $seq"
    log "resource fee:    +$RESOURCE_FEE"

    # Output is 9 lines: restore env, restore hash, upload env, upload hash,
    # create env, create hash, invoke env, invoke hash, ConfigUpgradeSetKey.
    # The secret goes in on stdin, never as an argument.
    output="$(printf '%s\n' "$SECRET_KEY" \
        | "$CORE_BIN" get-settings-upgrade-txs "$PUBLIC_KEY" "$seq" "$PASSPHRASE" \
            --xdr "$xdr" --signtxs --add-resource-fee "$RESOURCE_FEE")" \
        || die "stellar-core get-settings-upgrade-txs failed"
    [ "$(printf '%s\n' "$output" | wc -l | tr -d ' ')" -eq 9 ] \
        || die "unexpected get-settings-upgrade-txs output"

    PHASE_KEY="$(printf '%s\n' "$output" | sed -n '9p')"
    log "ConfigUpgradeSetKey: $PHASE_KEY"

    if [ "$DRY_RUN" = "true" ]; then
        log "dry run: would submit restore $(printf '%s\n' "$output" | sed -n '2p'),"
        log "  upload $(printf '%s\n' "$output" | sed -n '4p'), create $(printf '%s\n' "$output" | sed -n '6p'),"
        log "  invoke $(printf '%s\n' "$output" | sed -n '8p')"
        return 0
    fi

    step "$label: submitting transactions"
    # Consecutive sequence numbers, so they must be applied in order.
    submit_tx "1/4 restore" "$(printf '%s\n' "$output" | sed -n '1p')" "$(printf '%s\n' "$output" | sed -n '2p')"
    submit_tx "2/4 upload"  "$(printf '%s\n' "$output" | sed -n '3p')" "$(printf '%s\n' "$output" | sed -n '4p')"
    submit_tx "3/4 create"  "$(printf '%s\n' "$output" | sed -n '5p')" "$(printf '%s\n' "$output" | sed -n '6p')"
    submit_tx "4/4 invoke"  "$(printf '%s\n' "$output" | sed -n '7p')" "$(printf '%s\n' "$output" | sed -n '8p')"
}

arm_and_wait()
{
    # Records the ledger before arming, so the search for the applying ledger
    # starts no later than the upgrade can.
    local key="$1" start
    start="$(latest_ledger)"
    arm_on_nodes "$key"
    wait_for_ledger_with_upgrade "$key" "$start"
}

# ---------------------------------------------------------------- preflight --

[ -n "${NODE_URLS// /}" ] || die "--node-urls is required: every futurenet validator's admin URL"

for tool in curl jq stellar-xdr xxd base64; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found in PATH"
done
if [ -z "$CORE_BIN" ]; then
    CORE_BIN="$(command -v stellar-core || true)"
fi
[ -n "$CORE_BIN" ] && [ -x "$CORE_BIN" ] || die "stellar-core not found; pass --core-bin PATH"
log "stellar-core: $("$CORE_BIN" version </dev/null 2>/dev/null | head -1)"
log "stellar-xdr:  $(stellar-xdr version | head -1)"
log "rpc:          $RPC_URL"

if [ -n "$SCHEDULE_ONLY_KEY" ]; then
    [ -z "$SETTINGS_FILES" ] || die "--schedule-only arms an already-submitted key; drop the settings files"
    check_nodes
    verify_on_nodes "$SCHEDULE_ONLY_KEY"
    if [ "$DRY_RUN" = "true" ]; then
        check_nothing_armed
        log "dry run: would arm $SCHEDULE_ONLY_KEY $UPGRADE_IN from now"
        exit 0
    fi
    arm_and_wait "$SCHEDULE_ONLY_KEY"
    step "Done"
    exit 0
fi

[ -n "$SETTINGS_FILES" ] || { usage; exit 1; }
TOTAL=0
for f in $SETTINGS_FILES; do TOTAL=$((TOTAL + 1)); done
if [ "$NO_SCHEDULE" = "true" ] && [ "$TOTAL" -gt 1 ]; then
    die "--no-schedule takes one settings file: a later phase must be built after the earlier one is applied"
fi

# Fetch and validate every file up front, so a typo in the last one fails
# before the first is applied. TMP_DIR is created here, in the top-level
# shell, because resolve_settings runs inside $(...).
for f in $SETTINGS_FILES; do
    if [ ! -e "$f" ]; then
        resolve_settings_ref
        TMP_DIR="$(mktemp -d)" || die "could not create a temp directory"
        trap cleanup EXIT
        break
    fi
done
step "Settings"
for f in $SETTINGS_FILES; do
    RESOLVED_FILES="$RESOLVED_FILES $(resolve_settings "$f")"
done
for f in $RESOLVED_FILES; do
    stellar-xdr encode --type ConfigUpgradeSet "$f" >/dev/null 2>&1 \
        || die "$f is not a valid ConfigUpgradeSet"
done

# Check the validators before submitting anything: finding out they can't be
# reached after four transactions are in would waste them.
check_nodes
if [ "$NO_SCHEDULE" = "false" ]; then
    check_nothing_armed
fi

# One account for every phase; each phase re-reads its sequence number.
setup_account

INDEX=0
for SETTINGS_JSON in $RESOLVED_FILES; do
    INDEX=$((INDEX + 1))
    LABEL="phase $INDEX/$TOTAL"

    generate_and_submit "$SETTINGS_JSON" "$LABEL"

    if [ "$DRY_RUN" = "true" ]; then
        if [ "$TOTAL" -gt 1 ] && [ "$INDEX" -eq "$TOTAL" ]; then
            log ""
            log "note: nothing is submitted in a dry run, so every phase above was"
            log "      built against the same sequence number and futurenet's current"
            log "      settings. In a real run each phase is built after the last applied."
        fi
        continue
    fi

    verify_on_nodes "$PHASE_KEY"

    if [ "$NO_SCHEDULE" = "true" ]; then
        log ""
        log "$LABEL: submitted and accepted by every validator, not armed"
        log "  arm it with: $0 --schedule-only $PHASE_KEY"
        continue
    fi

    arm_and_wait "$PHASE_KEY"
    log "$LABEL done: $(basename "$SETTINGS_JSON") applied"
done

step "Done"
