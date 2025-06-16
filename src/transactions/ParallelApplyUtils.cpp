// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ParallelApplyUtils.h"
#include "transactions/MutableTransactionResult.h"

namespace stellar
{

std::unordered_set<LedgerKey>
getReadWriteKeysForStage(ApplyStage const& stage)
{
    std::unordered_set<LedgerKey> res;

    for (auto const& txBundle : stage)
    {
        for (auto const& lk :
             txBundle.getTx()->sorobanResources().footprint.readWrite)
        {
            res.emplace(lk);
            if (isSorobanEntry(lk))
            {
                res.emplace(getTTLKey(lk));
            }
        }
    }
    return res;
}

std::unique_ptr<ThreadEntryMap>
collectEntries(ThreadEntryMap const& globalEntryMap, Cluster const& cluster)
{
    auto entryMap = std::make_unique<ThreadEntryMap>();
    for (auto const& txBundle : cluster)
    {
        auto const& footprint = txBundle.getTx()->sorobanResources().footprint;
        for (auto const& lk : footprint.readWrite)
        {
            auto it = globalEntryMap.find(lk);
            if (it != globalEntryMap.end())
            {
                // If the entry exists, we take it
                entryMap->emplace(lk, it->second);
            }
        }

        for (auto const& lk : footprint.readOnly)
        {
            auto it = globalEntryMap.find(lk);
            if (it != globalEntryMap.end())
            {
                // If the entry exists, we take it
                entryMap->emplace(lk, it->second);
            }
        }
    }

    return entryMap;
}

void
preParallelApplyAndCollectModifiedClassicEntries(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages, ThreadEntryMap& globalEntryMap)
{
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));

    auto fetchInMemoryClassicEntries =
        [&](xdr::xvector<LedgerKey> const& keys) {
            for (auto const& lk : keys)
            {
                if (isSorobanEntry(lk))
                {
                    continue;
                }

                auto entryPair = ltx.getNewestVersionBelowRoot(lk);
                if (!entryPair.first)
                {
                    continue;
                }

                std::optional<LedgerEntry> entry =
                    entryPair.second ? std::make_optional<LedgerEntry>(
                                           entryPair.second->ledgerEntry())
                                     : std::nullopt;

                globalEntryMap.emplace(lk, ThreadEntry{entry, false});
            }
        };

    // First call preParallelApply on all transactions,
    // and then load from footprints. This order is important
    // because preParallelApply modifies the fee source accounts
    // and those accounts could show up in the footprint
    // of a different transaction.
    for (auto const& stage : stages)
    {
        for (auto const& txBundle : stage)
        {
            // Make sure to call preParallelApply on all txs because this will
            // modify the fee source accounts sequence numbers.
            txBundle.getTx()->preParallelApply(app, ltx,
                                               txBundle.getEffects().getMeta(),
                                               txBundle.getResPayload());
        }
    }

    for (auto const& stage : stages)
    {
        for (auto const& txBundle : stage)
        {
            auto const& footprint =
                txBundle.getTx()->sorobanResources().footprint;

            fetchInMemoryClassicEntries(footprint.readWrite);
            fetchInMemoryClassicEntries(footprint.readOnly);
        }
    }
}

void
setDelta(SearchableSnapshotConstPtr liveSnapshot,
         ThreadEntryMap const& entryMap,
         OpModifiedEntryMap const& opModifiedEntryMap,
         ParallelLedgerInfo const& ledgerInfo, TxEffects& effects)
{
    for (auto const& newUpdates : opModifiedEntryMap)
    {
        auto const& lk = newUpdates.first;
        auto const& le = newUpdates.second;

        // Any key the op updates should also be in entryMap because the
        // keys were taken from the footprint (the ttl keys were added
        // as well)

        auto prevLe = getLiveEntry(lk, liveSnapshot, entryMap);

        LedgerTxnDelta::EntryDelta entryDelta;
        if (prevLe)
        {
            entryDelta.previous =
                std::make_shared<InternalLedgerEntry>(*prevLe);
        }
        if (le)
        {
            auto deltaLe = *le;
            // This is for the invariants check in LedgerManager
            deltaLe.lastModifiedLedgerSeq = ledgerInfo.getLedgerSeq();

            entryDelta.current = std::make_shared<InternalLedgerEntry>(deltaLe);
        }

        effects.setDeltaEntry(lk, entryDelta);
    }
}

std::optional<LedgerEntry>
getLiveEntry(LedgerKey const& lk, SearchableSnapshotConstPtr liveSnapshot,
             ThreadEntryMap const& entryMap)
{
    // TODO: These copies aren't ideal.
    auto entryIter = entryMap.find(lk);
    if (entryIter != entryMap.end())
    {
        return entryIter->second.mLedgerEntry;
    }
    else
    {
        auto res = liveSnapshot->load(lk);
        return res ? std::make_optional(*res) : std::nullopt;
    }
}
}