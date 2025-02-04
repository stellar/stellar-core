#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTxn.h"
#include "overlay/StellarXDR.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{
class LedgerKeyMeter;
bool isLive(LedgerEntry const& e, uint32_t cutoffLedger);

LedgerKey getTTLKey(LedgerEntry const& e);
LedgerKey getTTLKey(LedgerKey const& e);

// Precondition: The keys associated with entries are unique and constitute a
// subset of keys
template <typename KeySetT>
UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(KeySetT const& keys,
                      std::vector<LedgerEntry> const& entries,
                      LedgerKeyMeter* lkMeter = nullptr)
{
    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>> res;

    for (auto const& le : entries)
    {
        auto key = LedgerEntryKey(le);

        // Abort if two entries for the same key appear.
        releaseAssert(res.find(key) == res.end());

        // Only return entries for keys that were actually requested.
        if (keys.find(key) != keys.end())
        {
            res.emplace(key, std::make_shared<LedgerEntry const>(le));
        }
    }

    for (auto const& key : keys)
    {
        // If the key was not loaded (but not due to metering), we should put
        // a nullptr entry in the result.
        if (res.find(key) == res.end() &&
            (!lkMeter || !lkMeter->loadFailed(key)))
        {
            res.emplace(key, nullptr);
        }
    }
    return res;
}

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