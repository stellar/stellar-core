// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "crypto/SHA.h"
#include "util/GlobalChecks.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/types.h"

namespace stellar
{

template <typename KeySetT>
UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(KeySetT const& keys,
                      std::vector<LedgerEntry> const& entries)
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
        if (res.find(key) == res.end())
        {
            res.emplace(key, nullptr);
        }
    }
    return res;
}

template UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(LedgerKeySet const& keys,
                      std::vector<LedgerEntry> const& entries);

template UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(UnorderedSet<LedgerKey> const& keys,
                      std::vector<LedgerEntry> const& entries);

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
};