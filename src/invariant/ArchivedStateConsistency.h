#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"

namespace stellar
{
class Application;

class ArchivedStateConsistency : public Invariant
{
    std::string checkEvictionInvariants(
        UnorderedMap<LedgerKey, LedgerEntry> const& preloadedLiveEntries,
        UnorderedMap<LedgerKey, HotArchiveBucketEntry> const&
            preloadedArchivedEntries,
        UnorderedSet<LedgerKey> const& deletedKeys,
        std::vector<LedgerEntry> const& archivedEntries, uint32_t ledgerSeq,
        uint32_t ledgerVer);
    std::string checkRestoreInvariants(
        UnorderedMap<LedgerKey, LedgerEntry> const& preloadedLiveEntries,
        UnorderedMap<LedgerKey, HotArchiveBucketEntry> const&
            preloadedArchivedEntries,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState,
        uint32_t ledgerSeq, uint32_t ledgerVer);

  public:
    ArchivedStateConsistency();
    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string checkOnLedgerCommit(
        SearchableSnapshotConstPtr lclLiveState,
        SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
        std::vector<LedgerEntry> const& evictedFromLive,
        std::vector<LedgerKey> const& deletedKeysFromLive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState)
        override;

    virtual std::string start(Application& app) override;
};
}
