#!/bin/sh

# This script extracts all the wasm blobs in a given ledger and writes
# them to a directory as separate files, each identified by its hash,
# as ${WASM_SHA256}.wasm. It requires specifying a stellar-core.cfg
# file and a directory in which to deposit the wasm files.

if [ "$#" -lt 1 ]; then
  echo "Usage: $0 <path/to/stellar-core.cfg> <output-dir-for-wasms>"
  exit 1
fi
CFG="$1"
DIR="$2"

CORE=src/stellar-core
if [ ! -x ${CORE} ]
then
   echo "cannot find ${CORE}"
   exit 1
fi

mkdir -p "${DIR}"

TMPJSON="${DIR}/tmp.json"
TMPWASM="${DIR}/tmp.wasm"

"${CORE}" --conf "${CFG}" \
	  dump-ledger \
	  --filter-query "data.type == 'CONTRACT_CODE'" \
	  --console \
	  --output-file "${TMPJSON}"

for BLOB in $(jq .entry.data.contractCode.code "${TMPJSON}")
do
    echo "${BLOB}" | xxd -r -p >"${TMPWASM}"
    SHA=$(sha256sum "${TMPWASM}" | cut -f 1 -d ' ')
    SHAWASM="${DIR}/${SHA}.wasm"
    mv "${TMPWASM}" "${SHAWASM}"
done

rm -f "${TMPJSON}" "${TMPWASM}"
ls -l "${DIR}"
