#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "xdr/Stellar-ledger.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

namespace stellar
{

class AppConnector;
class LiveBucket;
enum LedgerEntryType : std::int32_t;
struct LedgerTxnDelta;
struct Operation;
struct OperationResult;
struct LedgerKey;

// NOTE: The checkOn* functions should have a default implementation so that
//       more can be added in the future without requiring changes to all
//       derived classes.
class Invariant
{
    bool const mStrict;

  public:
    explicit Invariant(bool strict) : mStrict(strict)
    {
    }

    virtual ~Invariant()
    {
    }

    virtual std::string getName() const = 0;

    bool
    isStrict() const
    {
        return mStrict;
    }

    virtual std::string
    checkOnBucketApply(std::shared_ptr<LiveBucket const> bucket,
                       uint32_t oldestLedger, uint32_t newestLedger,
                       std::unordered_set<LedgerKey> const& shadowedKeys)
    {
        return std::string{};
    }

    virtual std::string
    checkAfterAssumeState(uint32_t newestLedger)
    {
        return std::string{};
    }

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta,
                          std::vector<ContractEvent> const& events,
                          AppConnector& app)
    {
        return std::string{};
    }

    virtual std::string
    checkOnLedgerCommit(
        SearchableSnapshotConstPtr lclLiveState,
        SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
        std::vector<LedgerEntry> const& persitentEvictedFromLive,
        std::vector<LedgerKey> const& tempAndTTLEvictedFromLive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState)
    {
        return std::string{};
    }

    virtual bool
    usesStateSnapshotInvariant() const
    {
        return false;
    }

    virtual std::string
    stateSnapshotInvariant(CompleteConstLedgerStatePtr ledgerState,
                           InMemorySorobanState const& inMemorySnapshot)
    {
        return std::string{};
    }

#ifdef BUILD_TESTS
    virtual void
    snapshotForFuzzer()
    {
    }

    virtual void
    resetForFuzzer()
    {
    }
#endif // BUILD_TESTS
};
}
