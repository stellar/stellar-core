// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "invariant/InvariantManager.h"
#include "util/ThreadAnnotations.h"
#include "util/Timer.h"
#include <map>
#include <vector>

namespace medida
{
class Counter;
}

namespace stellar
{
class MetricsRegistry;

class InvariantManagerImpl : public InvariantManager
{
    Config const& mConfig;
    std::map<std::string, std::shared_ptr<Invariant>> mInvariants;
    std::vector<std::shared_ptr<Invariant>> mEnabled;
    medida::Counter& mInvariantFailureCount;
    medida::Counter& mStateSnapshotInvariantSkipped;
    std::atomic<bool> mStateSnapshotInvariantRunning{false};
    std::atomic<bool> mShouldRunStateSnapshotInvariant{false};
    VirtualTimer mStateSnapshotTimer;

    struct InvariantFailureInformation
    {
        uint32_t lastFailedOnLedger;
        std::string lastFailedWithMessage;
    };

    Mutex mutable mFailureInformationMutex;
    std::map<std::string, InvariantFailureInformation>
        mFailureInformation GUARDED_BY(mFailureInformationMutex);

  public:
    InvariantManagerImpl(Application& app);

    virtual Json::Value getJsonInfo() override
        LOCKS_EXCLUDED(mFailureInformationMutex);

    virtual std::vector<std::string> getEnabledInvariants() const override;
    bool isBucketApplyInvariantEnabled() const override;

    virtual void checkOnOperationApply(Operation const& operation,
                                       OperationResult const& opres,
                                       LedgerTxnDelta const& ltxDelta,
                                       std::vector<ContractEvent> const& events,
                                       AppConnector& app) override;

    virtual void checkOnBucketApply(
        std::shared_ptr<LiveBucket const> bucket, uint32_t ledger,
        uint32_t level, bool isCurr,
        std::unordered_set<LedgerKey> const& shadowedKeys) override;

    virtual void checkOnLedgerCommit(
        SearchableSnapshotConstPtr lclLiveState,
        SearchableHotArchiveSnapshotConstPtr lclHotArchiveState,
        std::vector<LedgerEntry> const& persitentEvictedFromLive,
        std::vector<LedgerKey> const& tempAndTTLEvictedFromLive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
        UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState)
        override;

    virtual void checkAfterAssumeState(uint32_t newestLedger) override;

    virtual void
    registerInvariant(std::shared_ptr<Invariant> invariant) override;

    virtual void enableInvariant(std::string const& name) override;

    virtual void start(LedgerManager const& ledgerManager) override;

    bool shouldRunInvariantSnapshot() const override;

    void runStateSnapshotInvariant(
        CompleteConstLedgerStatePtr ledgerState,
        InMemorySorobanState const& inMemorySnapshot) override;

#ifdef BUILD_TESTS
    void snapshotForFuzzer() override;
    void resetForFuzzer() override;
#endif // BUILD_TESTS

  private:
    void onInvariantFailure(std::shared_ptr<Invariant> invariant,
                            std::string const& message, uint32_t ledger)
        LOCKS_EXCLUDED(mFailureInformationMutex);

    virtual void handleInvariantFailure(bool isStrict,
                                        std::string const& message) const;

    void scheduleSnapshotTimer();
    void snapshotTimerFired();
};
}
