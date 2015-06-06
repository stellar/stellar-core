// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "ApplicationImpl.h"

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "ledger/LedgerManager.h"
#include "herder/Herder.h"
#include "overlay/OverlayManager.h"
#include "bucket/BucketManager.h"
#include "history/HistoryManager.h"
#include "database/Database.h"
#include "process/ProcessManager.h"
#include "main/CommandHandler.h"
#include "simulation/LoadGenerator.h"
#include "medida/metrics_registry.h"
#include "medida/reporting/console_reporter.h"
#include "medida/meter.h"

#include "util/TmpDir.h"
#include "util/Logging.h"
#include "util/make_unique.h"

#include <set>
#include <string>

static const int SHUTDOWN_DELAY_SECONDS = 1;

namespace stellar
{

ApplicationImpl::ApplicationImpl(VirtualClock& clock, Config const& cfg)
    : mVirtualClock(clock)
    , mConfig(cfg)
    , mWorkerIOService(std::thread::hardware_concurrency())
    , mWork(make_unique<asio::io_service::work>(mWorkerIOService))
    , mWorkerThreads()
    , mStopSignals(clock.getIOService(), SIGINT)
    , mStopping(false)
    , mStoppingTimer(*this)
{
#ifdef SIGQUIT
    mStopSignals.add(SIGQUIT);
#endif
#ifdef SIGTERM
    mStopSignals.add(SIGTERM);
#endif

    unsigned t = std::thread::hardware_concurrency();
    LOG(INFO) << "Application constructing "
              << "(worker threads: " << t << ")";
    mStopSignals.async_wait([this](asio::error_code const& ec, int sig)
                            {
                                if (!ec)
                                {
                                    LOG(INFO) << "got signal " << sig
                                              << ", shutting down";
                                    this->gracefulStop();
                                }
                            });

    // These must be constructed _after_ because they frequently call back
    // into App.getFoo() to get information / start up.
    mMetrics = make_unique<medida::MetricsRegistry>();
    mDatabase = make_unique<Database>(*this);
    mPersistentState = make_unique<PersistentState>(*this);

    bool initializeDB =
        (mConfig.REBUILD_DB || mConfig.DATABASE == "sqlite3://:memory:");
    if (initializeDB)
    {
        auto wipeMsg = (getPersistentState().getState(
                            PersistentState::kDatabaseInitialized) == "true"
                            ? " wiped and initialized"
                            : " initialized");

        mDatabase->initialize();

        LOG(INFO) << "* ";
        LOG(INFO) << "* The database has been" << wipeMsg;
        LOG(INFO) << "* ";
    }
    else if (mPersistentState->getState(
                 PersistentState::kForceSCPOnNextLaunch) == "true")
    {
        mConfig.FORCE_SCP = true;
    }

    mTmpDirManager = make_unique<TmpDirManager>(cfg.TMP_DIR_PATH);
    mOverlayManager = OverlayManager::create(*this);
    mLedgerManager = LedgerManager::create(*this);
    mHerder = Herder::create(*this);
    mBucketManager = BucketManager::create(*this);
    mHistoryManager = HistoryManager::create(*this);
    mProcessManager = ProcessManager::create(*this);
    mCommandHandler = make_unique<CommandHandler>(*this);

    while (t--)
    {
        mWorkerThreads.emplace_back([this, t]()
                                    {
                                        this->runWorkerThread(t);
                                    });
    }

    if (initializeDB)
    {
        mLedgerManager->startNewLedger();
    }

    LOG(INFO) << "Application constructed";
}

void
ApplicationImpl::reportCfgMetrics()
{
    if (!mMetrics)
    {
        return;
    }

    std::set<std::string> metricsToReport;
    std::set<std::string> allMetrics;
    for (auto& kv : mMetrics->GetAllMetrics())
    {
        allMetrics.insert(kv.first.ToString());
    }

    bool reportAvailableMetrics = false;
    for (auto const& name : mConfig.REPORT_METRICS)
    {
        if (allMetrics.find(name) == allMetrics.end())
        {
            LOG(INFO) << "";
            LOG(WARNING) << "Metric not found: " << name;
            reportAvailableMetrics = true;
        }
        metricsToReport.insert(name);
    }

    if (reportAvailableMetrics)
    {
        LOG(INFO) << "Available metrics: ";
        for (auto const& n : allMetrics)
        {
            LOG(INFO) << "    " << n;
        }
        LOG(INFO) << "";
    }

    std::ostringstream oss;
    medida::reporting::ConsoleReporter reporter(*mMetrics, oss);
    for (auto& kv : mMetrics->GetAllMetrics())
    {
        auto name = kv.first;
        auto metric = kv.second;
        auto nstr = name.ToString();
        if (metricsToReport.find(nstr) != metricsToReport.end())
        {
            LOG(INFO) << "";
            LOG(INFO) << "metric '" << nstr << "':";
            metric->Process(reporter);
            std::istringstream iss(oss.str());
            char buf[128];
            while (iss.getline(buf, 128))
            {
                LOG(INFO) << std::string(buf);
            }
            oss.str("");
            LOG(INFO) << "";
        }
    }
}

ApplicationImpl::~ApplicationImpl()
{
    LOG(INFO) << "Application destructing";
    reportCfgMetrics();
    shutdownMainIOService();
    joinAllThreads();
    LOG(INFO) << "Application destroyed";
}

uint64_t
ApplicationImpl::timeNow()
{
    return VirtualClock::to_time_t(getClock().now());
}

void
ApplicationImpl::start()
{
    if (mConfig.QUORUM_SET.threshold == 0)
    {
        throw std::invalid_argument("Quorum not configured");
    }
    mOverlayManager->start();

    if (mPersistentState->getState(PersistentState::kDatabaseInitialized) !=
        "true")
    {
        throw std::runtime_error(
            "Database not initialized and REBUID_DB is false.");
    }

    bool done = false;
    mLedgerManager->loadLastKnownLedger(
        [this, &done](asio::error_code const& ec)
        {
            if (mConfig.FORCE_SCP)
            {
                std::string flagClearedMsg = "";
                if (mPersistentState->getState(
                        PersistentState::kForceSCPOnNextLaunch) == "true")
                {
                    flagClearedMsg = " (`force scp` flag cleared in the db)";
                    mPersistentState->setState(
                        PersistentState::kForceSCPOnNextLaunch, "false");
                }

                LOG(INFO) << "* ";
                LOG(INFO) << "* Force-starting scp from the current db state."
                          << flagClearedMsg;
                LOG(INFO) << "* ";

                mHerder->bootstrap();
            }
            done = true;
        });

    while (!done && mVirtualClock.crank(false) > 0)
        ;
}

void
ApplicationImpl::runWorkerThread(unsigned i)
{
    mWorkerIOService.run();
}

void
ApplicationImpl::gracefulStop()
{
    if (mStopping)
    {
        return;
    }
    mStopping = true;
    if (mOverlayManager)
    {
        mOverlayManager->shutdown();
    }

    mStoppingTimer.expires_from_now(
        std::chrono::seconds(SHUTDOWN_DELAY_SECONDS));

    mStoppingTimer.async_wait(
        std::bind(&ApplicationImpl::shutdownMainIOService, this),
        VirtualTimer::onFailureNoop);
}

void
ApplicationImpl::shutdownMainIOService()
{
    if (!mVirtualClock.getIOService().stopped())
    {
        // Drain all events; things are shutting down.
        while (mVirtualClock.cancelAllEvents())
            ;
        mVirtualClock.getIOService().stop();
    }
}

void
ApplicationImpl::joinAllThreads()
{
    // We never strictly stop the worker IO service, just release the work-lock
    // that keeps the worker threads alive. This gives them the chance to finish
    // any work that the main thread queued.
    if (mWork)
    {
        mWork.reset();
    }
    LOG(DEBUG) << "Joining " << mWorkerThreads.size() << " worker threads";
    for (auto& w : mWorkerThreads)
    {
        w.join();
    }
    LOG(DEBUG) << "Joined all " << mWorkerThreads.size() << " threads";
}

bool
ApplicationImpl::manualClose()
{
    if (mConfig.MANUAL_CLOSE)
    {
        mHerder->triggerNextLedger(mLedgerManager->getLastClosedLedgerNum() +
                                   1);
        return true;
    }
    return false;
}

void
ApplicationImpl::generateLoad(uint32_t nAccounts,
                              uint32_t nTxs,
                              uint32_t txRate)
{
    if (!mLoadGenerator)
    {
        mLoadGenerator = make_unique<LoadGenerator>();
    }
    getMetrics().NewMeter({"loadgen", "run", "start"}, "run").Mark();
    mLoadGenerator->generateLoad(*this, nAccounts, nTxs, txRate);
}

void
ApplicationImpl::applyCfgCommands()
{
    for (auto cmd : mConfig.COMMANDS)
    {
        mCommandHandler->manualCmd(cmd);
    }
}

Config const&
ApplicationImpl::getConfig()
{
    return mConfig;
}

Application::State
ApplicationImpl::getState() const
{
    State s;
    if (mStopping)
    {
        s = APP_STOPPING_STATE;
    }
    else if (mHerder->getState() == Herder::HERDER_SYNCING_STATE)
    {
        s = APP_ACQUIRING_CONSENSUS_STATE;
    }
    else
    {
        switch (mLedgerManager->getState())
        {
        case LedgerManager::LM_BOOTING_STATE:
            // not sure it's worth exposing this to the user?
            s = APP_ACQUIRING_CONSENSUS_STATE;
            break;
        case LedgerManager::LM_CATCHING_UP_STATE:
            s = APP_CATCHING_UP_STATE;
            break;
        case LedgerManager::LM_SYNCED_STATE:
            s = APP_SYNCED_STATE;
            break;
        default:
            abort();
        }
    }
    return s;
}

std::string
ApplicationImpl::getStateHuman() const
{
    static const char* stateStrings[APP_NUM_STATE] = {
        "Booting", "Joining SCP", "Catching up", "Synced!", "Stopping"};
    return std::string(stateStrings[getState()]);
}

bool
ApplicationImpl::isStopping() const
{
    return mStopping;
}

VirtualClock&
ApplicationImpl::getClock()
{
    return mVirtualClock;
}

medida::MetricsRegistry&
ApplicationImpl::getMetrics()
{
    return *mMetrics;
}

TmpDirManager&
ApplicationImpl::getTmpDirManager()
{
    return *mTmpDirManager;
}

LedgerManager&
ApplicationImpl::getLedgerManager()
{
    return *mLedgerManager;
}

BucketManager&
ApplicationImpl::getBucketManager()
{
    return *mBucketManager;
}

HistoryManager&
ApplicationImpl::getHistoryManager()
{
    return *mHistoryManager;
}

ProcessManager&
ApplicationImpl::getProcessManager()
{
    return *mProcessManager;
}

Herder&
ApplicationImpl::getHerder()
{
    return *mHerder;
}

OverlayManager&
ApplicationImpl::getOverlayManager()
{
    return *mOverlayManager;
}

Database&
ApplicationImpl::getDatabase()
{
    return *mDatabase;
}

PersistentState&
ApplicationImpl::getPersistentState()
{
    return *mPersistentState;
}

asio::io_service&
ApplicationImpl::getWorkerIOService()
{
    return mWorkerIOService;
}
}
