// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <limits>
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
#include "invariant/SponsorshipCountIsValid.h"
#include "ledger/InMemoryLedgerTxnRoot.h"
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

#include <Tracy.hpp>
#include <fmt/format.h>
#include <set>
#include <string>

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
    , mPostOnBackgroundThreadDelay(
          mMetrics->NewTimer({"app", "post-on-background-thread", "delay"}))
    , mStartedOn(clock.system_now())
{
#ifdef SIGQUIT
    mStopSignals.add(SIGQUIT);
#endif
#ifdef SIGTERM
    mStopSignals.add(SIGTERM);
#endif

    std::srand(static_cast<uint32>(clock.now().time_since_epoch().count()));

    mNetworkID = sha256(mConfig.NETWORK_PASSPHRASE);

    TracyAppInfo(STELLAR_CORE_VERSION.c_str(), STELLAR_CORE_VERSION.size());
    TracyAppInfo(mConfig.NETWORK_PASSPHRASE.c_str(),
                 mConfig.NETWORK_PASSPHRASE.size());
    std::string nodeStr("Node: ");
    nodeStr += mConfig.NODE_SEED.getStrKeyPublic();
    TracyAppInfo(nodeStr.c_str(), nodeStr.size());
    std::string homeStr("HomeDomain: ");
    homeStr += mConfig.NODE_HOME_DOMAIN;
    TracyAppInfo(homeStr.c_str(), homeStr.size());

    mStopSignals.async_wait([this](asio::error_code const& ec, int sig) {
        if (!ec)
        {
            LOG_INFO(DEFAULT_LOG, "got signal {}, shutting down", sig);

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
    LOG_DEBUG(DEFAULT_LOG, "Application constructing (worker threads: {})", t);
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
    mDatabase = createDatabase();
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

#ifdef BEST_OFFER_DEBUGGING
    auto const bestOfferDebuggingEnabled = mConfig.BEST_OFFER_DEBUGGING_ENABLED;
#endif

    if (getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        mLedgerTxnRoot = std::make_unique<InMemoryLedgerTxnRoot>(
#ifdef BEST_OFFER_DEBUGGING
            bestOfferDebuggingEnabled
#endif
        );
        mNeverCommittingLedgerTxn =
            std::make_unique<LedgerTxn>(*mLedgerTxnRoot);
    }
    else
    {
        if (mConfig.ENTRY_CACHE_SIZE < 20000)
        {
            LOG_WARNING(DEFAULT_LOG,
                        "ENTRY_CACHE_SIZE({}) is below the recommended minimum "
                        "of 20000",
                        mConfig.ENTRY_CACHE_SIZE);
        }
        mLedgerTxnRoot = std::make_unique<LedgerTxnRoot>(
            *mDatabase, mConfig.ENTRY_CACHE_SIZE, mConfig.PREFETCH_BATCH_SIZE
#ifdef BEST_OFFER_DEBUGGING
            ,
            bestOfferDebuggingEnabled
#endif
        );
    }

    BucketListIsConsistentWithDatabase::registerInvariant(*this);
    AccountSubEntriesCountIsValid::registerInvariant(*this);
    ConservationOfLumens::registerInvariant(*this);
    LedgerEntryIsValid::registerInvariant(*this);
    LiabilitiesMatchOffers::registerInvariant(*this);
    SponsorshipCountIsValid::registerInvariant(*this);
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
    LOG_DEBUG(DEFAULT_LOG, "Application constructed");
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
            LOG_INFO(DEFAULT_LOG, "");
            LOG_WARNING(DEFAULT_LOG, "Metric not found: {}", name);
            reportAvailableMetrics = true;
        }
        metricsToReport.insert(name);
    }

    if (reportAvailableMetrics)
    {
        LOG_INFO(DEFAULT_LOG,
                 "Update REPORT_METRICS value in configuration file to "
                 "only include available metrics:");
        for (auto const& n : allMetrics)
        {
            LOG_INFO(DEFAULT_LOG, "    {}", n);
        }
        LOG_INFO(DEFAULT_LOG, "");
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
            LOG_INFO(DEFAULT_LOG, "");
            LOG_INFO(DEFAULT_LOG, "metric '{}':", nstr);
            metric->Process(reporter);
            std::istringstream iss(oss.str());
            char buf[128];
            while (iss.getline(buf, 128))
            {
                LOG_INFO(DEFAULT_LOG, "{}", std::string(buf));
            }
            oss.str("");
            LOG_INFO(DEFAULT_LOG, "");
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
    info["startedOn"] = VirtualClock::systemPointToISOString(mStartedOn);
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
    LOG_INFO(DEFAULT_LOG, "info -> {}", getJsonInfo().toStyledString());
}

Hash const&
ApplicationImpl::getNetworkID() const
{
    return mNetworkID;
}

ApplicationImpl::~ApplicationImpl()
{
    LOG_INFO(DEFAULT_LOG, "Application destructing");
    try
    {
        shutdownWorkScheduler();
        if (mProcessManager)
        {
            mProcessManager->shutdown();
        }
        if (mBucketManager)
        {
            mBucketManager->shutdown();
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR(DEFAULT_LOG, "Exception occurred while shutting down: {}",
                  e.what());
    }
    reportCfgMetrics();
    shutdownMainIOContext();
    joinAllThreads();
    LOG_INFO(DEFAULT_LOG, "Application destroyed");
}

uint64_t
ApplicationImpl::timeNow()
{
    return VirtualClock::to_time_t(getClock().system_now());
}

void
ApplicationImpl::start()
{
    if (mStarted)
    {
        CLOG_INFO(Ledger, "Skipping application start up");
        return;
    }
    CLOG_INFO(Ledger, "Starting up application");
    mStarted = true;

    if (mConfig.TESTING_UPGRADE_DATETIME.time_since_epoch().count() != 0)
    {
        mHerder->setUpgrades(mConfig);
    }

    if (mConfig.FORCE_SCP && !mConfig.NODE_IS_VALIDATOR)
    {
        throw std::invalid_argument(
            "FORCE_SCP is set but NODE_IS_VALIDATOR not set");
    }

    auto const isNetworkedValidator =
        mConfig.NODE_IS_VALIDATOR && !mConfig.RUN_STANDALONE;

    if (mConfig.METADATA_OUTPUT_STREAM != "" && isNetworkedValidator)
    {
        throw std::invalid_argument(
            "METADATA_OUTPUT_STREAM is set, NODE_IS_VALIDATOR is set, and "
            "RUN_STANDALONE is not set");
    }

    if (isNetworkedValidator)
    {
        if (mConfig.MODE_USES_IN_MEMORY_LEDGER)
        {
            throw std::invalid_argument(
                "MODE_USES_IN_MEMORY_LEDGER is set, NODE_IS_VALIDATOR is set, "
                "and RUN_STANDALONE is not set");
        }
    }

    if (getHistoryArchiveManager().hasAnyWritableHistoryArchive())
    {
        if (!mConfig.MODE_STORES_HISTORY)
        {
            throw std::invalid_argument("MODE_STORES_HISTORY is not set, but "
                                        "some history archives are writable");
        }
    }

    if (mConfig.QUORUM_SET.threshold == 0)
    {
        throw std::invalid_argument("Quorum not configured");
    }

    mConfig.logBasicInfo();

    bool done = false;
    mLedgerManager->loadLastKnownLedger([this,
                                         &done](asio::error_code const& ec) {
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
        if (mConfig.MODE_AUTO_STARTS_OVERLAY)
        {
            mOverlayManager->start();
        }
        auto npub = mHistoryManager->publishQueuedHistory();
        if (npub != 0)
        {
            CLOG_INFO(Ledger, "Restarted publishing {} queued snapshots", npub);
        }
        if (mConfig.FORCE_SCP)
        {
            LOG_INFO(DEFAULT_LOG, "* ");
            LOG_INFO(DEFAULT_LOG,
                     "* Force-starting scp from the current db state.");
            LOG_INFO(DEFAULT_LOG, "* ");

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
    if (mHerder)
    {
        mHerder->shutdown();
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
    LOG_DEBUG(DEFAULT_LOG, "Joining {} worker threads", mWorkerThreads.size());
    for (auto& w : mWorkerThreads)
    {
        w.join();
    }
    LOG_DEBUG(DEFAULT_LOG, "Joined all {} threads", mWorkerThreads.size());
}

std::string
ApplicationImpl::manualClose(optional<uint32_t> const& manualLedgerSeq,
                             optional<TimePoint> const& manualCloseTime)
{
    assertThreadIsMain();

    // Manual close only makes sense for validating nodes
    if (!mConfig.NODE_IS_VALIDATOR)
    {
        throw std::invalid_argument(
            "Issuing a manual ledger close requires NODE_IS_VALIDATOR to be "
            "set to true.");
    }

    if (mConfig.MANUAL_CLOSE)
    {
        auto const targetLedgerSeq =
            targetManualCloseLedgerSeqNum(manualLedgerSeq);

        if (mConfig.RUN_STANDALONE)
        {
            setManualCloseVirtualTime(manualCloseTime);
            advanceToLedgerBeforeManualCloseTarget(targetLedgerSeq);
        }
        else
        {
            // These may only be specified if RUN_STANDALONE is configured.
            releaseAssert(!manualCloseTime);
            releaseAssert(!manualLedgerSeq);
        }

        mHerder->triggerNextLedger(targetLedgerSeq, true);

        if (mConfig.RUN_STANDALONE)
        {
            auto const newLedgerSeq =
                getLedgerManager().getLastClosedLedgerNum();
            if (newLedgerSeq != targetLedgerSeq)
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING(
                        "Standalone manual close failed to produce expected "
                        "sequence number increment (expected {}; got {})"),
                    targetLedgerSeq, newLedgerSeq));
            }

            return fmt::format(
                FMT_STRING("Manually closed ledger with sequence "
                           "number {} and closeTime {}"),
                targetLedgerSeq,
                getLedgerManager()
                    .getLastClosedLedgerHeader()
                    .header.scpValue.closeTime);
        }

        return fmt::format(
            FMT_STRING("Manually triggered a ledger close with sequence "
                       "number {}"),
            targetLedgerSeq);
    }
    else if (!mConfig.FORCE_SCP)
    {
        mHerder->setInSyncAndTriggerNextLedger();
        return "Triggered a new consensus round";
    }

    throw std::invalid_argument(
        "Set MANUAL_CLOSE=true in the stellar-core.cfg if you want to "
        "close every ledger manually. Otherwise, run stellar-core "
        "with --wait-for-consensus flag to close ledger once and "
        "trigger consensus. Ensure NODE_IS_VALIDATOR is set to true.");
}

uint32_t
ApplicationImpl::targetManualCloseLedgerSeqNum(
    optional<uint32_t> const& explicitlyProvidedSeqNum)
{
    auto const startLedgerSeq = getLedgerManager().getLastClosedLedgerNum();

    // The "scphistory" stores ledger sequence numbers as INTs.
    if (startLedgerSeq >=
        static_cast<uint32>(std::numeric_limits<int32_t>::max()))
    {
        throw std::invalid_argument(fmt::format(
            FMT_STRING(
                "Manually closed ledger sequence number ({}) already at max"),
            startLedgerSeq));
    }

    auto const nextLedgerSeq = startLedgerSeq + 1;

    if (explicitlyProvidedSeqNum)
    {
        if (*explicitlyProvidedSeqNum >
            static_cast<uint32>(std::numeric_limits<int32_t>::max()))
        {
            // The "scphistory" stores ledger sequence numbers as INTs.
            throw std::invalid_argument(fmt::format(
                FMT_STRING("Manual close ledger sequence number {} beyond max"),
                *explicitlyProvidedSeqNum,
                std::numeric_limits<int32_t>::max()));
        }

        if (*explicitlyProvidedSeqNum <= startLedgerSeq)
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("Invalid manual close ledger sequence number {} "
                           "(must exceed current sequence number {})"),
                *explicitlyProvidedSeqNum, startLedgerSeq));
        }
    }

    return explicitlyProvidedSeqNum ? *explicitlyProvidedSeqNum : nextLedgerSeq;
}

void
ApplicationImpl::setManualCloseVirtualTime(
    optional<TimePoint> const& explicitlyProvidedCloseTime)
{
    TimePoint constexpr firstSecondOfYear2200GMT = 7'258'118'400ULL;

    static_assert(
        firstSecondOfYear2200GMT <=
            std::min(static_cast<TimePoint>(
                         std::chrono::duration_cast<std::chrono::seconds>(
                             VirtualClock::system_time_point::duration::max())
                             .count()),
                     static_cast<TimePoint>(
                         std::numeric_limits<std::time_t>::max())) -
                Herder::MAX_TIME_SLIP_SECONDS.count(),
        "Documented limit (first second of year 2200 GMT) of manual "
        "close time would allow runtime overflow in Herder");

    TimePoint const lastCloseTime = getLedgerManager()
                                        .getLastClosedLedgerHeader()
                                        .header.scpValue.closeTime;
    uint64_t const now = VirtualClock::to_time_t(getClock().system_now());
    uint64_t nextCloseTime = std::max(now, lastCloseTime + 1);

    TimePoint const maxCloseTime = firstSecondOfYear2200GMT;

    if (explicitlyProvidedCloseTime)
    {
        if (*explicitlyProvidedCloseTime < nextCloseTime)
        {
            throw std::invalid_argument(
                fmt::format(FMT_STRING("Manual close time {} too early (last "
                                       "close time={}, now={})"),
                            *explicitlyProvidedCloseTime, lastCloseTime, now));
        }
        nextCloseTime = *explicitlyProvidedCloseTime;
    }
    else
    {
        // Without an explicitly-provided closeTime, imitate what
        // HerderImpl::triggerNextLedger() would do in REAL_TIME
        // (unless that would move the virtual clock backwards).
        uint64_t const defaultNextCloseTime =
            VirtualClock::to_time_t(std::chrono::system_clock::now());
        nextCloseTime = std::max(nextCloseTime, defaultNextCloseTime);
    }

    if (nextCloseTime > maxCloseTime)
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("New close time {} would exceed max ({})"),
                        nextCloseTime, maxCloseTime));
    }

    getClock().setCurrentVirtualTime(VirtualClock::from_time_t(nextCloseTime));
}

void
ApplicationImpl::advanceToLedgerBeforeManualCloseTarget(
    uint32_t const& targetLedgerSeq)
{
    if (targetLedgerSeq != getLedgerManager().getLastClosedLedgerNum() + 1)
    {
        LOG_INFO(DEFAULT_LOG,
                 "manually advancing to ledger with sequence number {} to "
                 "prepare to close number {} (last ledger to close was number "
                 "{})",
                 targetLedgerSeq - 1, targetLedgerSeq,
                 getLedgerManager().getLastClosedLedgerNum());

        LedgerTxn ltx(getLedgerTxnRoot());
        ltx.loadHeader().current().ledgerSeq = targetLedgerSeq - 1;
        getLedgerManager().manuallyAdvanceLedgerHeader(
            ltx.loadHeader().current());
        ltx.commit();

        getHerder().forceSCPStateIntoSyncWithLastClosedLedger();
    }
}

#ifdef BUILD_TESTS
void
ApplicationImpl::generateLoad(bool isCreate, uint32_t nAccounts,
                              uint32_t offset, uint32_t nTxs, uint32_t txRate,
                              uint32_t batchSize,
                              std::chrono::seconds spikeInterval,
                              uint32_t spikeSize)
{
    getMetrics().NewMeter({"loadgen", "run", "start"}, "run").Mark();
    getLoadGenerator().generateLoad(isCreate, nAccounts, offset, nTxs, txRate,
                                    batchSize, spikeInterval, spikeSize);
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

    // Update action-queue related metrics
    int64_t qsize = static_cast<int64_t>(getClock().getActionQueueSize());
    mMetrics->NewCounter({"process", "action", "queue"}).set_count(qsize);
    TracyPlot("process.action.queue", qsize);
    mMetrics->NewCounter({"process", "action", "overloaded"})
        .set_count(static_cast<int64_t>(getClock().actionQueueIsOverloaded()));
}

void
ApplicationImpl::syncAllMetrics()
{
    mHerder->syncMetrics();
    mLedgerManager->syncMetrics();
    mCatchupManager->syncMetrics();
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
ApplicationImpl::postOnMainThread(std::function<void()>&& f, std::string&& name,
                                  Scheduler::ActionType type)
{
    LogSlowExecution isSlow{name, LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    mVirtualClock.postAction(
        [this, f = std::move(f), isSlow]() {
            mPostOnMainThreadDelay.Update(isSlow.checkElapsedTime());
            f();
        },
        std::move(name), type);
}

void
ApplicationImpl::postOnBackgroundThread(std::function<void()>&& f,
                                        std::string jobName)
{
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    asio::post(getWorkerIOContext(), [this, f = std::move(f), isSlow]() {
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

std::unique_ptr<Database>
ApplicationImpl::createDatabase()
{
    return std::make_unique<Database>(*this);
}

AbstractLedgerTxnParent&
ApplicationImpl::getLedgerTxnRoot()
{
    assertThreadIsMain();
    return mConfig.MODE_USES_IN_MEMORY_LEDGER ? *mNeverCommittingLedgerTxn
                                              : *mLedgerTxnRoot;
}
}
