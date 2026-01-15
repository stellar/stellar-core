// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "crypto/SHA.h"
#include "ledger/NetworkConfig.h"
#include "rust/RustBridge.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include "xdr/Stellar-types.h"
#include <fmt/format.h>

namespace stellar
{

bool
isLive(LedgerEntry const& e, uint32_t cutoffLedger)
{
    releaseAssert(e.data.type() == TTL);
    return e.data.ttl().liveUntilLedgerSeq >= cutoffLedger;
}

LedgerKey
getTTLKey(LedgerEntry const& e)
{
    return getTTLKey(LedgerEntryKey(e));
}

LedgerKey
getTTLKey(LedgerKey const& e)
{
    releaseAssert(e.type() == CONTRACT_CODE || e.type() == CONTRACT_DATA);
    LedgerKey k;
    k.type(TTL);
    k.ttl().keyHash = sha256(xdr::xdr_to_opaque(e));
    return k;
}

LedgerEntry
getTTLEntryForTTLKey(LedgerKey const& ttlKey, uint32_t ttl)
{
    releaseAssert(ttlKey.type() == TTL);
    LedgerEntry ttlEntry;
    ttlEntry.data.type(TTL);
    ttlEntry.data.ttl().keyHash = ttlKey.ttl().keyHash;
    ttlEntry.data.ttl().liveUntilLedgerSeq = ttl;
    return ttlEntry;
}

uint32_t
ledgerEntrySizeForRent(LedgerEntry const& entry, uint32_t entryXdrSize,
                       uint32_t ledgerVersion,
                       SorobanNetworkConfig const& sorobanConfig)
{
    bool isCodeEntry = isContractCodeEntry(entry.data);
    uint32_t entrySizeForRent = entryXdrSize;

    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23) &&
        isCodeEntry)
    {
        uint32_t memorySize = rust_bridge::contract_code_memory_size_for_rent(
            Config::CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
            toCxxBuf(entry.data.contractCode()),
            toCxxBuf(sorobanConfig.cpuCostParams()),
            toCxxBuf(sorobanConfig.memCostParams()));
        uint64_t totalSize = static_cast<uint64_t>(entrySizeForRent) +
                             static_cast<uint64_t>(memorySize);
        entrySizeForRent = static_cast<uint32_t>(std::min(
            totalSize,
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
    }
    return entrySizeForRent;
}
};
