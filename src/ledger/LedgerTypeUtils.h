#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "util/UnorderedMap.h"
#include "util/XDROperators.h"

namespace stellar
{
bool isLive(LedgerEntry const& e, uint32_t cutoffLedger);

LedgerKey getTTLKey(LedgerEntry const& e);
LedgerKey getTTLKey(LedgerKey const& e);

// Precondition: The keys associated with entries are unique and constitute a
// subset of keys
template <typename KeySetT>
UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(KeySetT const& keys,
                      std::vector<LedgerEntry> const& entries);

template <typename T>
bool
isSorobanEntry(T const& e)
{
    return e.type() == CONTRACT_DATA || e.type() == CONTRACT_CODE;
}

template <typename T>
bool
isTemporaryEntry(T const& e)
{
    return e.type() == CONTRACT_DATA &&
           e.contractData().durability == ContractDataDurability::TEMPORARY;
}

template <typename T>
bool
isPersistentEntry(T const& e)
{
    return e.type() == CONTRACT_CODE ||
           (e.type() == CONTRACT_DATA &&
            e.contractData().durability == PERSISTENT);
}
}