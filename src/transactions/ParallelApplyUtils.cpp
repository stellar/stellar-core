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
collectEntries(AppConnector& app, AbstractLedgerTxn& ltx,
               Cluster const& cluster)
{
    auto entryMap = std::make_unique<ThreadEntryMap>();
    for (auto const& txBundle : cluster)
    {
        if (txBundle.getResPayload().isSuccess())
        {
            txBundle.getTx()->preloadEntriesForParallelApply(
                app, app.getSorobanMetrics(), ltx, *entryMap,
                txBundle.getResPayload(),
                txBundle.getEffects().getMeta().getDiagnosticEventManager());
        }
    }

    return entryMap;
}
}