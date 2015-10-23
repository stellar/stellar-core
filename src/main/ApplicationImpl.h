#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "Application.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include <thread>
#include "medida/timer_context.h"

namespace medida
{
class Counter;
class Timer;
}

namespace stellar
{
class TmpDirManager;
class LedgerManager;
class Herder;
class BucketManager;
class HistoryManager;
class ProcessManager;
class CommandHandler;
class Database;
class LoadGenerator;

class ApplicationImpl : public Application
{
  public:
    ApplicationImpl(VirtualClock& clock, Config const& cfg);
    virtual ~ApplicationImpl() override;

    virtual uint64_t timeNow() override;

    virtual Config const& getConfig() override;

    virtual State getState() const override;
    virtual std::string getStateHuman() const override;
    virtual std::string getExtraStateInfo() const override;
    virtual void setExtraStateInfo(std::string const& stateStr) override;
    virtual bool isStopping() const override;
    virtual VirtualClock& getClock() override;
    virtual medida::MetricsRegistry& getMetrics() override;
    virtual void syncOwnMetrics() override;
    virtual void syncAllMetrics() override;
    virtual TmpDirManager& getTmpDirManager() override;
    virtual LedgerManager& getLedgerManager() override;
    virtual BucketManager& getBucketManager() override;
    virtual HistoryManager& getHistoryManager() override;
    virtual ProcessManager& getProcessManager() override;
    virtual Herder& getHerder() override;
    virtual OverlayManager& getOverlayManager() override;
    virtual Database& getDatabase() override;
    virtual PersistentState& getPersistentState() override;
    virtual CommandHandler& getCommandHandler() override;

    virtual asio::io_service& getWorkerIOService() override;

    void newDB() override;
    virtual void start() override;

    // Stops the worker io_service, which should cause the threads to exit once
    // they finish running any work-in-progress. If you want a more abrupt exit
    // than this, call exit() and hope for the best.
    virtual void gracefulStop() override;

    // Wait-on and join all the threads this application started; should only
    // return when there is no more work to do or someone has force-stopped the
    // worker io_service. Application can be safely destroyed after this
    // returns.
    virtual void joinAllThreads() override;

    virtual bool manualClose() override;

    virtual void generateLoad(uint32_t nAccounts, uint32_t nTxs,
                              uint32_t txRate, bool autoRate) override;

    virtual void checkDB() override;

    virtual void applyCfgCommands() override;

    virtual void reportCfgMetrics() override;

    virtual void reportInfo() override;

    virtual Hash const& getNetworkID() const override;

  private:
    VirtualClock& mVirtualClock;
    Config mConfig;

    // NB: The io_service should come first, then the 'manager' sub-objects,
    // then the threads. Do not reorder these fields.
    //
    // The fields must be constructed in this order, because the subsystem
    // objects need to be fully instantiated before any workers acquire
    // references to them.
    //
    // The fields must be destructed in the reverse order because all worker
    // threads must be joined and destroyed before we start tearing down
    // subsystems.

    asio::io_service mWorkerIOService;
    std::unique_ptr<asio::io_service::work> mWork;

    std::unique_ptr<Database> mDatabase;
    std::unique_ptr<TmpDirManager> mTmpDirManager;
    std::unique_ptr<OverlayManager> mOverlayManager;
    std::unique_ptr<LedgerManager> mLedgerManager;
    std::unique_ptr<Herder> mHerder;
    std::unique_ptr<BucketManager> mBucketManager;
    std::unique_ptr<HistoryManager> mHistoryManager;
    std::unique_ptr<ProcessManager> mProcessManager;
    std::unique_ptr<CommandHandler> mCommandHandler;
    std::unique_ptr<PersistentState> mPersistentState;
    std::unique_ptr<LoadGenerator> mLoadGenerator;

    std::vector<std::thread> mWorkerThreads;

    asio::signal_set mStopSignals;

    bool mStopping;

    VirtualTimer mStoppingTimer;

    std::unique_ptr<medida::MetricsRegistry> mMetrics;
    medida::Counter& mAppStateCurrent;
    medida::Timer& mAppStateChanges;
    VirtualClock::time_point mLastStateChange;

    std::string mExtraStateInfo;

    Hash mNetworkID;

    void shutdownMainIOService();
    void runWorkerThread(unsigned i);
};
}
