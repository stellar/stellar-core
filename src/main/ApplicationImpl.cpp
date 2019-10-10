// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "ApplicationImpl.h"

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/HerderPersistence.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantManager.h"
#include "invariant/LedgerEntryIsValid.h"
#include "invariant/LiabilitiesMatchOffers.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/CommandHandler.h"
#include "main/ExternalQueue.h"
#include "main/Maintainer.h"
#include "main/StellarCoreVersion.h"
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
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "util/Thread.h"
#include "util/TmpDir.h"
#include "work/WorkScheduler.h"

#ifdef BUILD_TESTS
#include "simulation/LoadGenerator.h"
#endif

#include <set>
#include <string>
#include <util/format.h>

static const int SHUTDOWN_DELAY_SECONDS = 1;

namespace stellar
{

ApplicationImpl::ApplicationImpl(VirtualClock& clock, Config const& cfg)
    : mVirtualClock(clock)
    , mConfig(cfg)
    , mWorkerIOContext(mConfig.WORKER_THREADS)
    , mWork(std::make_unique<asio::io_context::work>(mWorkerIOContext))
    , mWorkerThreads()
    , mStopSignals(clock.getIOContext(), SIGINT)
    , mStarted(false)
    , mStopping(false)
    , mStoppingTimer(*this)
    , mMetrics(std::make_unique<medida::MetricsRegistry>())
    , mAppStateCurrent(mMetrics->NewCounter({"app", "state", "current"}))
    , mPostOnMainThreadDelay(
          mMetrics->NewTimer({"app", "post-on-main-thread", "delay"}))
    , mPostOnMainThreadWithDelayDelay(mMetrics->NewTimer(
          {"app", "post-on-main-thread-with-delay", "delay"}))
    , mPostOnBackgroundThreadDelay(
          mMetrics->NewTimer({"app", "post-on-background-thread", "delay"}))
    , mStartedOn(clock.now())
{
#ifdef SIGQUIT
    mStopSignals.add(SIGQUIT);
#endif
#ifdef SIGTERM
    mStopSignals.add(SIGTERM);
#endif

    std::srand(static_cast<uint32>(clock.now().time_since_epoch().count()));

    mNetworkID = sha256(mConfig.NETWORK_PASSPHRASE);

    mStopSignals.async_wait([this](asio::error_code const& ec, int sig) {
        if (!ec)
        {
            LOG(INFO) << "got signal " << sig << ", shutting down";

#ifdef BUILD_TESTS
            if (mConfig.TEST_CASES_ENABLED)
            {
                exit(1);
            }
#endif
            this->gracefulStop();
        }
    });

    auto t = mConfig.WORKER_THREADS;
    LOG(DEBUG) << "Application constructing "
               << "(worker threads: " << t << ")";
    while (t--)
    {
        auto thread = std::thread{[this]() {
            runCurrentThreadWithLowPriority();
            mWorkerIOContext.run();
        }};
        mWorkerThreads.emplace_back(std::move(thread));
    }
}

void
ApplicationImpl::initialize(bool createNewDB)
{
    mDatabase = std::make_unique<Database>(*this);
    mPersistentState = std::make_unique<PersistentState>(*this);
    mOverlayManager = createOverlayManager();
    mLedgerManager = createLedgerManager();
    mHerder = createHerder();
    mHerderPersistence = HerderPersistence::create(*this);
    mBucketManager = BucketManager::create(*this);
    mCatchupManager = CatchupManager::create(*this);
    mHistoryArchiveManager = std::make_unique<HistoryArchiveManager>(*this);
    mHistoryManager = HistoryManager::create(*this);
    mInvariantManager = createInvariantManager();
    mMaintainer = std::make_unique<Maintainer>(*this);
    mCommandHandler = std::make_unique<CommandHandler>(*this);
    mWorkScheduler = WorkScheduler::create(*this);
    mBanManager = BanManager::create(*this);
    mStatusManager = std::make_unique<StatusManager>();
    mLedgerTxnRoot = std::make_unique<LedgerTxnRoot>(
        *mDatabase, mConfig.ENTRY_CACHE_SIZE, mConfig.BEST_OFFERS_CACHE_SIZE,
        mConfig.PREFETCH_BATCH_SIZE);

    BucketListIsConsistentWithDatabase::registerInvariant(*this);
    AccountSubEntriesCountIsValid::registerInvariant(*this);
    ConservationOfLumens::registerInvariant(*this);
    LedgerEntryIsValid::registerInvariant(*this);
    LiabilitiesMatchOffers::registerInvariant(*this);
    enableInvariantsFromConfig();

    if (createNewDB || mConfig.DATABASE.value == "sqlite3://:memory:")
    {
        newDB();
    }
    else
    {
        upgradeDB();
    }

    // Subtle: process manager should come to existence _after_ BucketManager
    // initialization and newDB run, as it relies on tmp dir created in the
    // constructor
    mProcessManager = ProcessManager::create(*this);
    LOG(DEBUG) << "Application constructed";
}

void
ApplicationImpl::newDB()
{
    mDatabase->initialize();
    mDatabase->upgradeToCurrentSchema();
    mBucketManager->dropAll();
    mLedgerManager->startNewLedger();
}

void
ApplicationImpl::upgradeDB()
{
    mDatabase->upgradeToCurrentSchema();
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
        LOG(INFO) << "Update REPORT_METRICS value in configuration file to "
                     "only include available metrics:";
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

    info["build"] = STELLAR_CORE_VERSION;
    info["protocol_version"] = getConfig().LEDGER_PROTOCOL_VERSION;
    info["state"] = getStateHuman();
    info["startedOn"] = VirtualClock::pointToISOString(mStartedOn);
    auto const& lcl = lm.getLastClosedLedgerHeader();
    info["ledger"]["num"] = (int)lcl.header.ledgerSeq;
    info["ledger"]["hash"] = binToHex(lcl.hash);
    info["ledger"]["closeTime"] = (Json::UInt64)lcl.header.scpValue.closeTime;
    info["ledger"]["version"] = lcl.header.ledgerVersion;
    info["ledger"]["baseFee"] = lcl.header.baseFee;
    info["ledger"]["baseReserve"] = lcl.header.baseReserve;
    info["ledger"]["maxTxSetSize"] = lcl.header.maxTxSetSize;
    info["ledger"]["age"] = (int)lm.secondsSinceLastLedgerClose();
    info["peers"]["pending_count"] = getOverlayManager().getPendingPeersCount();
    info["peers"]["authenticated_count"] =
        getOverlayManager().getAuthenticatedPeersCount();
    info["network"] = getConfig().NETWORK_PASSPHRASE;

    auto& statusMessages = getStatusManager();
    auto counter = 0;
    for (auto statusMessage : statusMessages)
    {
        info["status"][counter++] = statusMessage.second;
    }

    auto& herder = getHerder();

    auto& quorumInfo = info["quorum"];

    // Try to get quorum set info for the previous ledger closed by the
    // network.
    if (herder.getCurrentLedgerSeq() > 1)
    {
        quorumInfo =
            herder.getJsonQuorumInfo(getConfig().NODE_SEED.getPublicKey(), true,
                                     false, herder.getCurrentLedgerSeq() - 1);
    }

    // If the quorum set info for the previous ledger is missing, use the
    // current ledger
    auto qset = quorumInfo.get("qset", "");
    if (quorumInfo.empty() || qset.empty())
    {
        quorumInfo =
            herder.getJsonQuorumInfo(getConfig().NODE_SEED.getPublicKey(), true,
                                     false, herder.getCurrentLedgerSeq());
    }

    auto invariantFailures = getInvariantManager().getJsonInfo();
    if (!invariantFailures.empty())
    {
        info["invariant_failures"] = invariantFailures;
    }

    info["history_failure_rate"] =
        fmt::format("{:.2}", getHistoryArchiveManager().getFailureRate());

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
    shutdownWorkScheduler();
    if (mProcessManager)
    {
        mProcessManager->shutdown();
    }
    reportCfgMetrics();
    shutdownMainIOContext();
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
    if (mStarted)
    {
        CLOG(INFO, "Ledger") << "Skipping application start up";
        return;
    }
    CLOG(INFO, "Ledger") << "Starting up application";
    mStarted = true;

    if (mConfig.TESTING_UPGRADE_DATETIME.time_since_epoch().count() != 0)
    {
        mHerder->setUpgrades(mConfig);
    }

    if (mPersistentState->getState(PersistentState::kForceSCPOnNextLaunch) ==
        "true")
    {
        if (!mConfig.NODE_IS_VALIDATOR)
        {
            LOG(ERROR) << "Starting stellar-core in FORCE_SCP mode requires "
                          "NODE_IS_VALIDATOR to be set";
            throw std::invalid_argument("NODE_IS_VALIDATOR not set");
        }
        mConfig.FORCE_SCP = true;
    }

    if (mConfig.QUORUM_SET.threshold == 0)
    {
        throw std::invalid_argument("Quorum not configured");
    }

    mConfig.logBasicInfo();

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
            // set known cursors before starting maintenance job
            ExternalQueue ps(*this);
            ps.setInitialCursors(mConfig.KNOWN_CURSORS);
            mMaintainer->start();
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

    while (!done && mVirtualClock.crank(true))
        ;
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
    shutdownWorkScheduler();
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
        std::bind(&ApplicationImpl::shutdownMainIOContext, this),
        VirtualTimer::onFailureNoop);
}

void
ApplicationImpl::shutdownMainIOContext()
{
    if (!mVirtualClock.getIOContext().stopped())
    {
        // Drain all events; things are shutting down.
        while (mVirtualClock.cancelAllEvents())
            ;
        mVirtualClock.getIOContext().stop();
    }
}

void
ApplicationImpl::shutdownWorkScheduler()
{
    if (mWorkScheduler)
    {
        mWorkScheduler->shutdown();

        while (mWorkScheduler->getState() != BasicWork::State::WORK_ABORTED)
        {
            mVirtualClock.crank();
        }
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

#ifdef BUILD_TESTS
void
ApplicationImpl::generateLoad(bool isCreate, uint32_t nAccounts,
                              uint32_t offset, uint32_t nTxs, uint32_t txRate,
                              uint32_t batchSize)
{
    getMetrics().NewMeter({"loadgen", "run", "start"}, "run").Mark();
    getLoadGenerator().generateLoad(isCreate, nAccounts, offset, nTxs, txRate,
                                    batchSize);
}

LoadGenerator&
ApplicationImpl::getLoadGenerator()
{
    if (!mLoadGenerator)
    {
        mLoadGenerator = std::make_unique<LoadGenerator>(*this);
    }
    return *mLoadGenerator;
}
#endif

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

    if (!mStarted)
    {
        s = APP_CREATED_STATE;
    }
    else if (mStopping)
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

void
ApplicationImpl::clearMetrics(std::string const& domain)
{
    MetricResetter resetter;
    auto const& metrics = mMetrics->GetAllMetrics();
    for (auto const& kv : metrics)
    {
        if (domain.empty() || kv.first.domain() == domain)
        {
            kv.second->Process(resetter);
        }
    }
}

TmpDirManager&
ApplicationImpl::getTmpDirManager()
{
    return getBucketManager().getTmpDirManager();
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

HistoryArchiveManager&
ApplicationImpl::getHistoryArchiveManager()
{
    return *mHistoryArchiveManager;
}

HistoryManager&
ApplicationImpl::getHistoryManager()
{
    return *mHistoryManager;
}

Maintainer&
ApplicationImpl::getMaintainer()
{
    return *mMaintainer;
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

WorkScheduler&
ApplicationImpl::getWorkScheduler()
{
    return *mWorkScheduler;
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

asio::io_context&
ApplicationImpl::getWorkerIOContext()
{
    return mWorkerIOContext;
}

void
ApplicationImpl::postOnMainThread(std::function<void()>&& f,
                                  std::string jobName)
{
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    mVirtualClock.postToCurrentCrank([ this, f = std::move(f), isSlow ]() {
        mPostOnMainThreadDelay.Update(isSlow.checkElapsedTime());
        f();
    });
}

void
ApplicationImpl::postOnMainThreadWithDelay(std::function<void()>&& f,
                                           std::string jobName)
{
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    mVirtualClock.postToNextCrank([ this, f = std::move(f), isSlow ]() {
        mPostOnMainThreadWithDelayDelay.Update(isSlow.checkElapsedTime());
        f();
    });
}

void
ApplicationImpl::postOnBackgroundThread(std::function<void()>&& f,
                                        std::string jobName)
{
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    asio::post(getWorkerIOContext(), [ this, f = std::move(f), isSlow ]() {
        mPostOnBackgroundThreadDelay.Update(isSlow.checkElapsedTime());
        f();
    });
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

std::unique_ptr<LedgerManager>
ApplicationImpl::createLedgerManager()
{
    return LedgerManager::create(*this);
}

LedgerTxnRoot&
ApplicationImpl::getLedgerTxnRoot()
{
    assertThreadIsMain();
    return *mLedgerTxnRoot;
}
}
