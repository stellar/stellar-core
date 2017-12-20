// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "ApplicationImpl.h"

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "StellarCoreVersion.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/HerderPersistence.h"
#include "history/HistoryManager.h"
#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "invariant/CacheIsConsistentWithDatabase.h"
#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantManager.h"
#include "invariant/LedgerEntryIsValid.h"
#include "invariant/MinimumAccountBalance.h"
#include "ledger/LedgerManager.h"
#include "main/CommandHandler.h"
#include "main/ExternalQueue.h"
#include "main/NtpSynchronizationChecker.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/reporting/console_reporter.h"
#include "medida/timer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "process/ProcessManager.h"
#include "scp/LocalNode.h"
#include "scp/QuorumSetUtils.h"
#include "simulation/LoadGenerator.h"
#include "util/StatusManager.h"
#include "work/WorkManager.h"

#include "util/Logging.h"
#include "util/TmpDir.h"
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
    , mMetrics(make_unique<medida::MetricsRegistry>())
    , mAppStateCurrent(mMetrics->NewCounter({"app", "state", "current"}))
    , mAppStateChanges(mMetrics->NewTimer({"app", "state", "changes"}))
    , mLastStateChange(clock.now())
{
#ifdef SIGQUIT
    mStopSignals.add(SIGQUIT);
#endif
#ifdef SIGTERM
    mStopSignals.add(SIGTERM);
#endif

    std::srand(static_cast<uint32>(clock.now().time_since_epoch().count()));

    mNetworkID = sha256(mConfig.NETWORK_PASSPHRASE);

    unsigned t = std::thread::hardware_concurrency();
    LOG(DEBUG) << "Application constructing "
               << "(worker threads: " << t << ")";
    mStopSignals.async_wait([this](asio::error_code const& ec, int sig) {
        if (!ec)
        {
            LOG(INFO) << "got signal " << sig << ", shutting down";
            this->gracefulStop();
        }
    });

    while (t--)
    {
        mWorkerThreads.emplace_back([this, t]() { this->runWorkerThread(t); });
    }
}

void
ApplicationImpl::initialize()
{
    mDatabase = make_unique<Database>(*this);
    mPersistentState = make_unique<PersistentState>(*this);
    mTmpDirManager =
        make_unique<TmpDirManager>(mConfig.BUCKET_DIR_PATH + "/tmp");
    mOverlayManager = createOverlayManager();
    mLedgerManager = LedgerManager::create(*this);
    mHerder = createHerder();
    mHerderPersistence = HerderPersistence::create(*this);
    mBucketManager = BucketManager::create(*this);
    mCatchupManager = CatchupManager::create(*this);
    mHistoryManager = HistoryManager::create(*this);
    mInvariantManager = createInvariantManager();
    mProcessManager = ProcessManager::create(*this);
    mCommandHandler = make_unique<CommandHandler>(*this);
    mWorkManager = WorkManager::create(*this);
    mBanManager = BanManager::create(*this);
    mStatusManager = make_unique<StatusManager>();

    BucketListIsConsistentWithDatabase::registerInvariant(*this);
    AccountSubEntriesCountIsValid::registerInvariant(*this);
    CacheIsConsistentWithDatabase::registerInvariant(*this);
    ConservationOfLumens::registerInvariant(*this);
    LedgerEntryIsValid::registerInvariant(*this);
    MinimumAccountBalance::registerInvariant(*this);
    enableInvariantsFromConfig();

    if (!mConfig.NTP_SERVER.empty())
    {
        mNtpSynchronizationChecker =
            std::make_shared<NtpSynchronizationChecker>(*this,
                                                        mConfig.NTP_SERVER);
    }

    LOG(DEBUG) << "Application constructed";
}

void
ApplicationImpl::newDB()
{
    mDatabase->initialize();
    mDatabase->upgradeToCurrentSchema();

    LOG(INFO) << "* ";
    LOG(INFO) << "* The database has been initialized";
    LOG(INFO) << "* ";

    mLedgerManager->startNewLedger();
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

Json::Value
ApplicationImpl::getJsonInfo()
{
    auto root = Json::Value{};

    auto& lm = getLedgerManager();

    auto& info = root["info"];

    if (getConfig().UNSAFE_QUORUM)
        info["UNSAFE_QUORUM"] = "UNSAFE QUORUM ALLOWED";
    info["build"] = STELLAR_CORE_VERSION;
    info["protocol_version"] = getConfig().LEDGER_PROTOCOL_VERSION;
    info["state"] = getStateHuman();
    info["ledger"]["num"] = (int)lm.getLedgerNum();
    auto const& lcl = lm.getLastClosedLedgerHeader();
    info["ledger"]["hash"] = binToHex(lcl.hash);
    info["ledger"]["closeTime"] = (Json::UInt64)lcl.header.scpValue.closeTime;
    info["ledger"]["version"] = lcl.header.ledgerVersion;
    info["ledger"]["baseFee"] = lcl.header.baseFee;
    info["ledger"]["baseReserve"] = lcl.header.baseReserve;
    info["ledger"]["age"] = (int)lm.secondsSinceLastLedgerClose();
    info["pending_peers_count"] =
        (int)getOverlayManager().getPendingPeersCount();
    info["authenticated_peers_count"] =
        (int)getOverlayManager().getAuthenticatedPeersCount();
    info["network"] = getConfig().NETWORK_PASSPHRASE;

    auto& statusMessages = getStatusManager();
    auto counter = 0;
    for (auto statusMessage : statusMessages)
    {
        info["status"][counter++] = statusMessage.second;
    }

    auto& herder = getHerder();
    Json::Value q;
    herder.dumpQuorumInfo(q, getConfig().NODE_SEED.getPublicKey(), true,
                          herder.getCurrentLedgerSeq());
    if (q["slots"].size() != 0)
    {
        info["quorum"] = q["slots"];
    }

    Json::Value invariantFailures = getInvariantManager().getInformation();
    if (!invariantFailures.empty())
    {
        info["invariant_failures"] = invariantFailures;
    }

    return root;
}

void
ApplicationImpl::reportInfo()
{
    mLedgerManager->loadLastKnownLedger(nullptr);
    LOG(INFO) << "info -> " << getJsonInfo().toStyledString();
}

Hash const&
ApplicationImpl::getNetworkID() const
{
    return mNetworkID;
}

ApplicationImpl::~ApplicationImpl()
{
    LOG(INFO) << "Application destructing";
    if (mNtpSynchronizationChecker)
    {
        mNtpSynchronizationChecker->shutdown();
    }
    if (mProcessManager)
    {
        mProcessManager->shutdown();
    }
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
    mDatabase->upgradeToCurrentSchema();

    if (mConfig.TESTING_UPGRADE_DATETIME.time_since_epoch().count() != 0)
    {
        mHerder->setUpgrades(mConfig);
    }

    if (mPersistentState->getState(PersistentState::kForceSCPOnNextLaunch) ==
        "true")
    {
        mConfig.FORCE_SCP = true;
    }

    if (mConfig.QUORUM_SET.threshold == 0)
    {
        throw std::invalid_argument("Quorum not configured");
    }
    if (!isQuorumSetSane(mConfig.QUORUM_SET, !mConfig.UNSAFE_QUORUM))
    {
        std::string err("Invalid QUORUM_SET: duplicate entry or bad threshold "
                        "(should be between ");
        if (mConfig.UNSAFE_QUORUM)
        {
            err = err + "1";
        }
        else
        {
            err = err + "51";
        }
        err = err + " and 100)";
        throw std::invalid_argument(err);
    }

    bool done = false;
    mLedgerManager->loadLastKnownLedger(
        [this, &done](asio::error_code const& ec) {
            if (ec)
            {
                throw std::runtime_error(
                    "Unable to restore last-known ledger state");
            }

            // restores Herder's state before starting overlay
            mHerder->restoreState();
            // perform maintenance tasks if configured to do so
            // for now, we only perform it when CATCHUP_COMPLETE is not set
            if (mConfig.MAINTENANCE_ON_STARTUP && !mConfig.CATCHUP_COMPLETE)
            {
                maintenance();
            }
            mOverlayManager->start();
            auto npub = mHistoryManager->publishQueuedHistory();
            if (npub != 0)
            {
                CLOG(INFO, "Ledger")
                    << "Restarted publishing " << npub << " queued snapshots";
            }
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

    if (mNtpSynchronizationChecker)
    {
        mNtpSynchronizationChecker->start();
    }

    while (!done && mVirtualClock.crank(true))
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
    if (mNtpSynchronizationChecker)
    {
        mNtpSynchronizationChecker->shutdown();
    }
    if (mProcessManager)
    {
        mProcessManager->shutdown();
    }
    if (mBucketManager)
    {
        mBucketManager->shutdown();
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
ApplicationImpl::generateLoad(uint32_t nAccounts, uint32_t nTxs,
                              uint32_t txRate, bool autoRate)
{
    getMetrics().NewMeter({"loadgen", "run", "start"}, "run").Mark();
    getLoadGenerator().generateLoad(*this, nAccounts, nTxs, txRate, autoRate);
}

LoadGenerator&
ApplicationImpl::getLoadGenerator()
{
    if (!mLoadGenerator)
    {
        mLoadGenerator = make_unique<LoadGenerator>(getNetworkID());
    }
    return *mLoadGenerator;
}

void
ApplicationImpl::checkDB()
{
    getClock().getIOService().post([this] {
        checkDBAgainstBuckets(this->getMetrics(), this->getBucketManager(),
                              this->getDatabase(),
                              this->getBucketManager().getBucketList());
    });
}

void
ApplicationImpl::maintenance()
{
    LOG(INFO) << "Performing maintenance";
    ExternalQueue ps(*this);
    ps.process();
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
            s = APP_CONNECTED_STANDBY_STATE;
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
        "Booting",     "Joining SCP", "Connected",
        "Catching up", "Synced!",     "Stopping"};
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

void
ApplicationImpl::syncOwnMetrics()
{
    int64_t c = mAppStateCurrent.count();
    int64_t n = static_cast<int64_t>(getState());
    if (c != n)
    {
        mAppStateCurrent.set_count(n);
        auto now = mVirtualClock.now();
        mAppStateChanges.Update(now - mLastStateChange);
        mLastStateChange = now;
    }

    // Flush crypto pure-global-cache stats. They don't belong
    // to a single app instance but first one to flush will claim
    // them.
    uint64_t vhit = 0, vmiss = 0;
    PubKeyUtils::flushVerifySigCacheCounts(vhit, vmiss);
    mMetrics->NewMeter({"crypto", "verify", "hit"}, "signature").Mark(vhit);
    mMetrics->NewMeter({"crypto", "verify", "miss"}, "signature").Mark(vmiss);
    mMetrics->NewMeter({"crypto", "verify", "total"}, "signature")
        .Mark(vhit + vmiss);

    // Similarly, flush global process-table stats.
    mMetrics->NewCounter({"process", "memory", "handles"})
        .set_count(mProcessManager->getNumRunningProcesses());
}

void
ApplicationImpl::syncAllMetrics()
{
    mHerder->syncMetrics();
    mLedgerManager->syncMetrics();
    syncOwnMetrics();
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

CatchupManager&
ApplicationImpl::getCatchupManager()
{
    return *mCatchupManager;
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

HerderPersistence&
ApplicationImpl::getHerderPersistence()
{
    return *mHerderPersistence;
}

InvariantManager&
ApplicationImpl::getInvariantManager()
{
    return *mInvariantManager;
}

OverlayManager&
ApplicationImpl::getOverlayManager()
{
    return *mOverlayManager;
}

Database&
ApplicationImpl::getDatabase() const
{
    return *mDatabase;
}

PersistentState&
ApplicationImpl::getPersistentState()
{
    return *mPersistentState;
}

CommandHandler&
ApplicationImpl::getCommandHandler()
{
    return *mCommandHandler;
}

WorkManager&
ApplicationImpl::getWorkManager()
{
    return *mWorkManager;
}

BanManager&
ApplicationImpl::getBanManager()
{
    return *mBanManager;
}

StatusManager&
ApplicationImpl::getStatusManager()
{
    return *mStatusManager;
}

asio::io_service&
ApplicationImpl::getWorkerIOService()
{
    return mWorkerIOService;
}

void
ApplicationImpl::enableInvariantsFromConfig()
{
    for (auto name : mConfig.INVARIANT_CHECKS)
    {
        mInvariantManager->enableInvariant(name);
    }
}

std::unique_ptr<Herder>
ApplicationImpl::createHerder()
{
    return Herder::create(*this);
}

std::unique_ptr<InvariantManager>
ApplicationImpl::createInvariantManager()
{
    return InvariantManager::create(*this);
}

std::unique_ptr<OverlayManager>
ApplicationImpl::createOverlayManager()
{
    return OverlayManager::create(*this);
}
}
