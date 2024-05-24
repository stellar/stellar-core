#!/bin/sh

# This script extracts the soroban-env-host versions specified in src/rust/Cargo.toml
# and, for both curr and prev host versions, runs `check-lockfile-intersection`
# comparing the tree of dependencies rooted in the host version in this repo's Cargo.lock and
# the tree of dependencies in the soroban-env-host repo's Cargo.lock at the specified revision.
#
# The script will exit with a non-zero status if there are any differences between the two trees,
# and the differences will be printed. You can resolve the discrepancies one by one by running
#
#   cargo update <package-name> --precise <version>
#
# in whichever repository you want to align to the other.

set -e
SRCDIR=$(realpath $(dirname $0)/..)

if [ ! -f "${SRCDIR}/src/rust/Cargo.toml" ]
then
   echo "cannot find ${SRCDIR}/src/rust/Cargo.toml"
   exit 1
fi

echo "Examining soroban-env-host versions specified in ${SRCDIR}/src/rust/Cargo.toml"

cd "${SRCDIR}/src/rust"
CURR_REV=$(cargo read-manifest  | jq '.dependencies | map(select(.rename == "soroban-env-host-curr") | .source)[0] | capture("rev=(?<rev>[a-f0-9]+)$") | .rev')
PREV_REV=$(cargo read-manifest  | jq '.dependencies | map(select(.rename == "soroban-env-host-prev") | .source)[0] | capture("rev=(?<rev>[a-f0-9]+)$") | .rev')
CURR_REV=$(eval echo $CURR_REV)
PREV_REV=$(eval echo $PREV_REV)
cd "${SRCDIR}"

echo "Current soroban-env-host revision: $CURR_REV"
echo "Previous soroban-env-host revision: $PREV_REV"

EXCLUDES=tracy-client,getrandom,serde_json
SOROBAN_ENV_URL=https://raw.githubusercontent.com/stellar/rs-soroban-env

for REV in $CURR_REV $PREV_REV
do
    echo "Checking soroban-env-host revision: $REV in Cargo.lock against $SOROBAN_ENV_URL/$REV/Cargo.lock"
    check-lockfile-intersection Cargo.lock "${SOROBAN_ENV_URL}/$REV/Cargo.lock" --pkg-name-a soroban-env-host --pkg-hash-a $REV --pkg-name-b soroban-env-host --exclude-pkg-a $EXCLUDES
done
