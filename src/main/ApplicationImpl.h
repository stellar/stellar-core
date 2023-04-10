#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Application.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include "medida/timer_context.h"
#include "util/MetricResetter.h"
#include "util/Timer.h"
#include "xdr/Stellar-ledger-entries.h"
#include <optional>
#include <thread>

namespace medida
{
class Timer;
}

namespace stellar
{
class TmpDirManager;
class LedgerManager;
class Herder;
class HerderPersistence;
class BucketManager;
class HistoryManager;
class ProcessManager;
class CommandHandler;
class Database;
class LedgerTxn;
class LedgerTxnRoot;
class InMemoryLedgerTxn;
class InMemoryLedgerTxnRoot;
class LoadGenerator;

class ApplicationImpl : public Application
{
  public:
    ApplicationImpl(VirtualClock& clock, Config const& cfg);
    virtual ~ApplicationImpl() override;

    virtual void initialize(bool newDB, bool forceRebuild) override;

    virtual void resetLedgerState() override;

    virtual uint64_t timeNow() override;

    virtual Config const& getConfig() override;

    virtual State getState() const override;
    virtual std::string getStateHuman() const override;
    virtual bool isStopping() const override;
    virtual VirtualClock& getClock() override;
    virtual medida::MetricsRegistry& getMetrics() override;
    virtual void syncOwnMetrics() override;
    virtual void syncAllMetrics() override;
    virtual void clearMetrics(std::string const& domain) override;
    virtual TmpDirManager& getTmpDirManager() override;
    virtual LedgerManager& getLedgerManager() override;
    virtual BucketManager& getBucketManager() override;
    virtual CatchupManager& getCatchupManager() override;
    virtual HistoryArchiveManager& getHistoryArchiveManager() override;
    virtual HistoryManager& getHistoryManager() override;
    virtual Maintainer& getMaintainer() override;
    virtual ProcessManager& getProcessManager() override;
    virtual Herder& getHerder() override;
    virtual HerderPersistence& getHerderPersistence() override;
    virtual InvariantManager& getInvariantManager() override;
    virtual OverlayManager& getOverlayManager() override;
    virtual Database& getDatabase() const override;
    virtual PersistentState& getPersistentState() override;
    virtual CommandHandler& getCommandHandler() override;
    virtual WorkScheduler& getWorkScheduler() override;
    virtual BanManager& getBanManager() override;
    virtual StatusManager& getStatusManager() override;

    virtual asio::io_context& getWorkerIOContext() override;
    virtual void postOnMainThread(std::function<void()>&& f, std::string&& name,
                                  Scheduler::ActionType type) override;
    virtual void postOnBackgroundThread(std::function<void()>&& f,
                                        std::string jobName) override;

    virtual void start() override;

    // Stops the worker io_context, which should cause the threads to exit once
    // they finish running any work-in-progress. If you want a more abrupt exit
    // than this, call exit() and hope for the best.
    virtual void gracefulStop() override;

    // Wait-on and join all the threads this application started; should only
    // return when there is no more work to do or someone has force-stopped the
    // worker io_context. Application can be safely destroyed after this
    // returns.
    virtual void joinAllThreads() override;

    virtual void validateAndLogConfig() override;

    virtual std::string
    manualClose(std::optional<uint32_t> const& manualLedgerSeq,
                std::optional<TimePoint> const& manualCloseTime) override;

#ifdef BUILD_TESTS
    virtual void generateLoad(GeneratedLoadConfig cfg) override;

    virtual LoadGenerator& getLoadGenerator() override;
#endif

    virtual void applyCfgCommands() override;

    virtual void reportCfgMetrics() override;

    virtual Json::Value getJsonInfo(bool verbose) override;

    virtual void reportInfo(bool verbose) override;

    virtual std::shared_ptr<BasicWork>
    scheduleSelfCheck(bool waitUntilNextCheckpoint) override;

    virtual Hash const& getNetworkID() const override;

    virtual AbstractLedgerTxnParent& getLedgerTxnRoot() override;

    virtual void resetDBForInMemoryMode() override;

  protected:
    std::unique_ptr<LedgerManager>
        mLedgerManager;              // allow to change that for tests
    std::unique_ptr<Herder> mHerder; // allow to change that for tests

  private:
    VirtualClock& mVirtualClock;
    Config mConfig;

    // NB: The io_context should come first, then the 'manager' sub-objects,
    // then the threads. Do not reorder these fields.
    //
    // The fields must be constructed in this order, because the subsystem
    // objects need to be fully instantiated before any workers acquire
    // references to them.
    //
    // The fields must be destructed in the reverse order because all worker
    // threads must be joined and destroyed before we start tearing down
    // subsystems.

    asio::io_context mWorkerIOContext;
    std::unique_ptr<asio::io_context::work> mWork;

    std::unique_ptr<BucketManager> mBucketManager;
    std::unique_ptr<Database> mDatabase;
    std::unique_ptr<OverlayManager> mOverlayManager;
    std::unique_ptr<CatchupManager> mCatchupManager;
    std::unique_ptr<HerderPersistence> mHerderPersistence;
    std::unique_ptr<HistoryArchiveManager> mHistoryArchiveManager;
    std::unique_ptr<HistoryManager> mHistoryManager;
    std::unique_ptr<InvariantManager> mInvariantManager;
    std::unique_ptr<Maintainer> mMaintainer;
    std::shared_ptr<ProcessManager> mProcessManager;
    std::shared_ptr<WorkScheduler> mWorkScheduler;
    std::unique_ptr<PersistentState> mPersistentState;
    std::unique_ptr<BanManager> mBanManager;
    std::unique_ptr<StatusManager> mStatusManager;
    std::unique_ptr<AbstractLedgerTxnParent> mLedgerTxnRoot;

    // These two exist for use in MODE_USES_IN_MEMORY_LEDGER only: the
    // mInMemoryLedgerTxnRoot is a _stub_ AbstractLedgerTxnParent that refuses
    // all commits and answers null to all queries; then an inner
    // "never-committing" sub-LedgerTxn is constructed beneath it that serves as
    // the "effective" in-memory root transaction, is returned when a client
    // requests the root.
    //
    // Note that using this only works when the ledger can fit in RAM -- as it
    // is held in the never-committing LedgerTxn in its entirety -- so if it
    // ever grows beyond RAM-size you need to use a mode with some sort of
    // database on secondary storage.
    std::unique_ptr<InMemoryLedgerTxnRoot> mInMemoryLedgerTxnRoot;
    std::unique_ptr<InMemoryLedgerTxn> mNeverCommittingLedgerTxn;

    std::unique_ptr<CommandHandler> mCommandHandler;

#ifdef BUILD_TESTS
    std::unique_ptr<LoadGenerator> mLoadGenerator;
#endif

    std::vector<std::thread> mWorkerThreads;

    asio::signal_set mStopSignals;

    bool mStarted;
    bool mStopping;

    VirtualTimer mStoppingTimer;
    VirtualTimer mSelfCheckTimer;

    std::unique_ptr<medida::MetricsRegistry> mMetrics;
    medida::Timer& mPostOnMainThreadDelay;
    medida::Timer& mPostOnBackgroundThreadDelay;
    VirtualClock::system_time_point mStartedOn;

    Hash mNetworkID;

    // A handle to any running self-check, to avoid scheduling
    // more than one concurrently.
    std::weak_ptr<BasicWork> mRunningSelfCheck;

    void newDB();

    void shutdownMainIOContext();
    void shutdownWorkScheduler();

    void enableInvariantsFromConfig();

    virtual std::unique_ptr<Herder> createHerder();
    virtual std::unique_ptr<InvariantManager> createInvariantManager();
    virtual std::unique_ptr<OverlayManager> createOverlayManager();
    virtual std::unique_ptr<LedgerManager> createLedgerManager();
    virtual std::unique_ptr<Database> createDatabase();

    uint32_t targetManualCloseLedgerSeqNum(
        std::optional<uint32_t> const& explicitlyProvidedSeqNum);

    void setManualCloseVirtualTime(
        std::optional<TimePoint> const& explicitlyProvidedCloseTime);

    void
    advanceToLedgerBeforeManualCloseTarget(uint32_t const& targetLedgerSeq);

    void upgradeToCurrentSchemaAndMaybeRebuildLedger(bool applyBuckets,
                                                     bool forceRebuild);
};
}
