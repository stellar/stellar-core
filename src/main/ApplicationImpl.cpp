// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#define STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "ApplicationImpl.h"

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "ledger/LedgerMaster.h"
#include "herder/Herder.h"
#include "overlay/OverlayGateway.h"
#include "overlay/PeerMaster.h"
#include "clf/CLFManager.h"
#include "history/HistoryMaster.h"
#include "database/Database.h"
#include "process/ProcessMaster.h"
#include "main/CommandHandler.h"
#include "medida/metrics_registry.h"
#include "medida/reporting/console_reporter.h"

#include "util/TmpDir.h"
#include "util/Logging.h"
#include "util/make_unique.h"

#include <set>
#include <string>

namespace stellar
{

ApplicationImpl::ApplicationImpl(VirtualClock& clock, Config const& cfg)
    : mState(Application::State::BOOTING_STATE)
    , mVirtualClock(clock)
    , mConfig(cfg)
    , mWorkerIOService(std::thread::hardware_concurrency())
    , mWork(make_unique<asio::io_service::work>(mWorkerIOService))
    , mWorkerThreads()
    , mStopSignals(clock.getIOService(), SIGINT)
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
                                LOG(INFO) << "got signal " << sig
                                          << ", shutting down";
                                this->gracefulStop();
                            });

    // These must be constructed _after_ because they frequently call back
    // into App.getFoo() to get information / start up.
    mMetrics = make_unique<medida::MetricsRegistry>();
    mDatabase = make_unique<Database>(*this);
    mPersistentState = make_unique<PersistentState>(*this);

    if (mPersistentState->getState(PersistentState::kForceSCPOnNextLaunch) ==
        "true")
    {
        mConfig.START_NEW_NETWORK = true;
    }

    // Initialize the db as early as possible, namely as soon as metrics,
    // database and persistentState are instantiated.
    if (mConfig.REBUILD_DB || mConfig.DATABASE == "sqlite3://:memory:")
    {
        mDatabase->initialize();
    }

    mTmpDirMaster = make_unique<TmpDirMaster>(cfg.TMP_DIR_PATH);
    mPeerMaster = make_unique<PeerMaster>(*this);
    mLedgerMaster = make_unique<LedgerMaster>(*this);
    mHerder = make_unique<Herder>(*this);
    mCLFManager = CLFManager::create(*this);
    mHistoryMaster = make_unique<HistoryMaster>(*this);
    mProcessMaster = make_unique<ProcessMaster>(*this);
    mCommandHandler = make_unique<CommandHandler>(*this);

    while (t--)
    {
        mWorkerThreads.emplace_back([this, t]()
                                    {
                                        this->runWorkerThread(t);
                                    });
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
    gracefulStop();
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
    if (mPersistentState->getState(PersistentState::kDatabaseInitialized) !=
        "true")
    {
        throw runtime_error("Database not initialized and REBUID_DB is false.");
    }

    bool hasLedger =
        !mPersistentState->getState(PersistentState::kLastClosedLedger).empty();

    if (mConfig.START_NEW_NETWORK)
    {
        string flagClearedMsg = "";
        if (mPersistentState->getState(
                PersistentState::kForceSCPOnNextLaunch) == "true")
        {
            flagClearedMsg = " (`force scp` flag cleared in the db)";
            mPersistentState->setState(PersistentState::kForceSCPOnNextLaunch,
                                       "false");
        }

        if (!hasLedger)
        {
            LOG(INFO) << "* ";
            LOG(INFO) << "* Force-starting scp from scratch, creating the "
                         "genesis ledger." << flagClearedMsg;
            LOG(INFO) << "* ";
            mLedgerMaster->startNewLedger();
        }
        else
        {
            LOG(INFO) << "* ";
            LOG(INFO) << "* Force-starting scp from the current db state."
                      << flagClearedMsg;
            LOG(INFO) << "* ";
            mLedgerMaster->loadLastKnownLedger();
        }
        mHerder->bootstrap();
    }
    else
    {
        mLedgerMaster->loadLastKnownLedger();
    }
}

void
ApplicationImpl::runWorkerThread(unsigned i)
{
    mWorkerIOService.run();
}

void
ApplicationImpl::gracefulStop()
{
    // Drain all events queued to fire on this app.
    while (mVirtualClock.cancelAllEventsFrom(*this))
        ;
    mVirtualClock.getIOService().stop();
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
        mHerder->triggerNextLedger();
        return true;
    }
    return false;
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
ApplicationImpl::getState()
{
    return mState;
}

void
ApplicationImpl::setState(State s)
{
    mState = s;
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

TmpDirMaster&
ApplicationImpl::getTmpDirMaster()
{
    return *mTmpDirMaster;
}

LedgerGateway&
ApplicationImpl::getLedgerGateway()
{
    return *mLedgerMaster;
}

LedgerMaster&
ApplicationImpl::getLedgerMaster()
{
    return *mLedgerMaster;
}

CLFManager&
ApplicationImpl::getCLFManager()
{
    return *mCLFManager;
}

HistoryMaster&
ApplicationImpl::getHistoryMaster()
{
    return *mHistoryMaster;
}

ProcessGateway&
ApplicationImpl::getProcessGateway()
{
    return *mProcessMaster;
}

HerderGateway&
ApplicationImpl::getHerderGateway()
{
    return *mHerder;
}

OverlayGateway&
ApplicationImpl::getOverlayGateway()
{
    return *mPeerMaster;
}

PeerMaster&
ApplicationImpl::getPeerMaster()
{
    return *mPeerMaster;
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
