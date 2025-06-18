#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
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
collectEntries(SearchableSnapshotConstPtr liveSnapshot,
               ThreadEntryMap const& globalEntryMap, Cluster const& cluster);

// sets LedgerTxnDelta within effects
void setDelta(SearchableSnapshotConstPtr liveSnapshot,
              ThreadEntryMap const& entryMap,
              OpModifiedEntryMap const& opModifiedEntryMap,
              UnorderedMap<LedgerKey, LedgerEntry> const& hotArchiveRestores,
              ParallelLedgerInfo const& ledgerInfo, TxEffects& effects);

void preParallelApplyAndCollectModifiedClassicEntries(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages, ThreadEntryMap& globalEntryMap);

std::optional<LedgerEntry> getLiveEntry(LedgerKey const& lk,
                                        SearchableSnapshotConstPtr liveSnapshot,
                                        ThreadEntryMap const& entryMap);

class LedgerAccessHelper
{
  public:
    // getLedgerEntry returns an entry if it exists, else nullopt.
    virtual std::optional<LedgerEntry>
    getLedgerEntryOpt(LedgerKey const& key) = 0;

    // upsert returns true if the entry was created, false if it was updated.
    virtual bool upsertLedgerEntry(LedgerKey const& key,
                                   LedgerEntry const& entry) = 0;

    // erase returns true if the entry was erased, false if it wasn't present.
    virtual bool eraseLedgerEntry(LedgerKey const& key) = 0;

    virtual uint32_t getLedgerVersion() = 0;
    virtual uint32_t getLedgerSeq() = 0;
};

class PreV23LedgerAccessHelper : virtual public LedgerAccessHelper
{
  protected:
    PreV23LedgerAccessHelper(AbstractLedgerTxn& ltx);

    AbstractLedgerTxn& mLtx;

    std::optional<LedgerEntry> getLedgerEntryOpt(LedgerKey const& key) override;
    bool upsertLedgerEntry(LedgerKey const& key,
                           LedgerEntry const& entry) override;
    bool eraseLedgerEntry(LedgerKey const& key) override;
    uint32_t getLedgerVersion() override;
    uint32_t getLedgerSeq() override;
};

class ParallelLedgerAccessHelper : virtual public LedgerAccessHelper
{

  protected:
    ParallelLedgerAccessHelper(ThreadEntryMap const& entryMap,
                               ParallelLedgerInfo const& ledgerInfo,
                               SearchableSnapshotConstPtr liveSnapshot);

    ThreadEntryMap const& mEntryMap;
    ParallelLedgerInfo const& mLedgerInfo;
    SearchableSnapshotConstPtr mLiveSnapshot;
    OpModifiedEntryMap mOpEntryMap;

    std::optional<LedgerEntry> getLedgerEntryOpt(LedgerKey const& key) override;
    bool upsertLedgerEntry(LedgerKey const& key,
                           LedgerEntry const& entry) override;
    bool eraseLedgerEntry(LedgerKey const& key) override;
    uint32_t getLedgerVersion() override;
    uint32_t getLedgerSeq() override;
};

}