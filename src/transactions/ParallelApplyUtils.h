#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "transactions/ParallelApplyStage.h"
#include "transactions/TransactionFrameBase.h"
#include <unordered_set>

namespace stellar
{

class ParallelLedgerInfo
{

  public:
    ParallelLedgerInfo(uint32_t version, uint32_t seq, uint32_t reserve,
                       TimePoint time, Hash const& id)
        : ledgerVersion(version)
        , ledgerSeq(seq)
        , baseReserve(reserve)
        , closeTime(time)
        , networkID(id)
    {
    }

    uint32_t
    getLedgerVersion() const
    {
        return ledgerVersion;
    }
    uint32_t
    getLedgerSeq() const
    {
        return ledgerSeq;
    }
    uint32_t
    getBaseReserve() const
    {
        return baseReserve;
    }
    TimePoint
    getCloseTime() const
    {
        return closeTime;
    }
    Hash
    getNetworkID() const
    {
        return networkID;
    }

  private:
    uint32_t ledgerVersion;
    uint32_t ledgerSeq;
    uint32_t baseReserve;
    TimePoint closeTime;
    Hash networkID;
};

std::unordered_set<LedgerKey> getReadWriteKeysForStage(ApplyStage const& stage);

std::unique_ptr<ThreadEntryMap>
collectEntries(ThreadEntryMap const& globalEntryMap, Cluster const& cluster);

// sets LedgerTxnDelta within effects
void setDelta(SearchableSnapshotConstPtr liveSnapshot,
              ThreadEntryMap const& entryMap,
              OpModifiedEntryMap const& opModifiedEntryMap,
              ParallelLedgerInfo const& ledgerInfo, TxEffects& effects);

void preParallelApplyAndCollectModifiedClassicEntries(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages, ThreadEntryMap& globalEntryMap);

std::optional<LedgerEntry> getLiveEntry(LedgerKey const& lk,
                                        SearchableSnapshotConstPtr liveSnapshot,
                                        ThreadEntryMap const& entryMap);

}