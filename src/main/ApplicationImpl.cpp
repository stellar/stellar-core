// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Fs.h"
#include "work/ConditionalWork.h"
#include "work/WorkWithCallback.h"
#include "xdr/Stellar-ledger-entries.h"
#include <limits>
#define STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "ApplicationImpl.h"

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/BucketManager.h"
#include "catchup/ApplyBucketsWork.h"
#include "crypto/Hex.h"
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
#include "invariant/ConstantProductInvariant.h"
#include "invariant/InvariantManager.h"
#include "invariant/LedgerEntryIsValid.h"
#include "invariant/LiabilitiesMatchOffers.h"
#include "invariant/SponsorshipCountIsValid.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/AppConnector.h"
#include "main/ApplicationUtils.h"
#include "main/CommandHandler.h"
#include "main/Maintainer.h"
#include "main/StellarCoreVersion.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/reporting/console_reporter.h"
#include "medida/timer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "process/ProcessManager.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "util/Thread.h"
#include "util/TmpDir.h"
#include "work/BasicWork.h"
#include "work/WorkScheduler.h"

#ifdef BUILD_TESTS
#include "ledger/test/InMemoryLedgerTxn.h"
#include "ledger/test/InMemoryLedgerTxnRoot.h"
#include "simulation/LoadGenerator.h"
#endif

#include <Tracy.hpp>
#include <fmt/format.h>
#include <optional>
#include <set>
#include <string>

static const int SHUTDOWN_DELAY_SECONDS = 1;

namespace stellar
{

ApplicationImpl::ApplicationImpl(VirtualClock& clock, Config const& cfg)
    : mVirtualClock(clock)
    , mConfig(cfg)
    // Allocate one worker to eviction when background eviction enabled
    , mWorkerIOContext(mConfig.WORKER_THREADS - 1)
    , mEvictionIOContext(std::make_unique<asio::io_context>(1))
    , mWork(std::make_unique<asio::io_context::work>(mWorkerIOContext))
    , mEvictionWork(
          mEvictionIOContext
              ? std::make_unique<asio::io_context::work>(*mEvictionIOContext)
              : nullptr)
    , mOverlayIOContext(mConfig.BACKGROUND_OVERLAY_PROCESSING
                            ? std::make_unique<asio::io_context>(1)
                            : nullptr)
    , mOverlayWork(mOverlayIOContext ? std::make_unique<asio::io_context::work>(
                                           *mOverlayIOContext)
                                     : nullptr)
    , mLedgerCloseIOContext(mConfig.parallelLedgerClose()
                                ? std::make_unique<asio::io_context>(1)
                                : nullptr)
    , mLedgerCloseWork(
          mLedgerCloseIOContext
              ? std::make_unique<asio::io_context::work>(*mLedgerCloseIOContext)
              : nullptr)
    , mWorkerThreads()
    , mEvictionThread()
    , mStopSignals(clock.getIOContext(), SIGINT)
    , mStarted(false)
    , mStopping(false)
    , mStoppingTimer(*this)
    , mSelfCheckTimer(*this)
    , mMetrics(
          std::make_unique<medida::MetricsRegistry>(cfg.HISTOGRAM_WINDOW_SIZE))
    , mPostOnMainThreadDelay(
          mMetrics->NewTimer({"app", "post-on-main-thread", "delay"}))
    , mPostOnBackgroundThreadDelay(
          mMetrics->NewTimer({"app", "post-on-background-thread", "delay"}))
    , mPostOnOverlayThreadDelay(
          mMetrics->NewTimer({"app", "post-on-overlay-thread", "delay"}))
    , mPostOnLedgerCloseThreadDelay(
          mMetrics->NewTimer({"app", "post-on-ledger-close-thread", "delay"}))
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

    releaseAssert(mConfig.WORKER_THREADS > 0);
    releaseAssert(mEvictionIOContext);

    // Allocate one thread for Eviction scan
    mEvictionThread = std::thread{[this]() {
        runCurrentThreadWithMediumPriority();
        mEvictionIOContext->run();
    }};

    --t;

    while (t--)
    {
        auto thread = std::thread{[this]() {
            runCurrentThreadWithLowPriority();
            mWorkerIOContext.run();
        }};
        mWorkerThreads.emplace_back(std::move(thread));
    }

    if (mConfig.BACKGROUND_OVERLAY_PROCESSING)
    {
        // Keep priority unchanged as overlay processes time-sensitive tasks
        mOverlayThread = std::thread{[this]() { mOverlayIOContext->run(); }};
    }

    if (mConfig.parallelLedgerClose())
    {
        mLedgerCloseThread =
            std::thread{[this]() { mLedgerCloseIOContext->run(); }};
    }
}

static void
maybeRebuildLedger(Application& app, bool applyBuckets)
{
    auto& ps = app.getPersistentState();
    if (ps.shouldRebuildForOfferTable())
    {
        app.getDatabase().clearPreparedStatementCache();
        soci::transaction tx(app.getDatabase().getRawSession());
        LOG_INFO(DEFAULT_LOG, "Dropping offers");
        app.getLedgerTxnRoot().dropOffers();
        tx.commit();

        // No transaction is needed. ApplyBucketsWork breaks the apply into many
        // small chunks, each of which has its own transaction. If it fails at
        // some point in the middle, then rebuildledger will not be cleared so
        // this will run again on next start up.
        if (applyBuckets)
        {
            LOG_INFO(DEFAULT_LOG,
                     "Rebuilding ledger tables by applying buckets");
            if (!applyBucketsForLCL(app))
            {
                throw std::runtime_error("Could not rebuild ledger tables");
            }
            LOG_INFO(DEFAULT_LOG, "Successfully rebuilt ledger tables");
        }
        LOG_INFO(DEFAULT_LOG, "Successfully rebuilt ledger tables");
    }

    ps.clearRebuildForOfferTable();
}

void
ApplicationImpl::initialize(bool createNewDB, bool forceRebuild)
{
    // Subtle: initialize the bucket manager first before initializing the
    // database. This is needed as some modes in core (such as in-memory) use a
    // small database inside the bucket directory.
    mBucketManager = BucketManager::create(*this);

    bool initNewDB =
        createNewDB || mConfig.DATABASE.value == "sqlite3://:memory:";
    if (initNewDB)
    {
        mBucketManager->dropAll();
    }

    mDatabase = createDatabase();
    mPersistentState = std::make_unique<PersistentState>(*this);
    mOverlayManager = createOverlayManager();
    mLedgerManager = createLedgerManager();
    mHerder = createHerder();
    mHerderPersistence = HerderPersistence::create(*this);
    mLedgerApplyManager = LedgerApplyManager::create(*this);
    mHistoryArchiveManager = std::make_unique<HistoryArchiveManager>(*this);
    mHistoryManager = HistoryManager::create(*this);
    mInvariantManager = createInvariantManager();
    mMaintainer = std::make_unique<Maintainer>(*this);
    mWorkScheduler = WorkScheduler::create(*this);
    mBanManager = BanManager::create(*this);
    mStatusManager = std::make_unique<StatusManager>();
    mAppConnector = std::make_unique<AppConnector>(*this);

    if (mConfig.ENTRY_CACHE_SIZE < 20000)
    {
        LOG_WARNING(DEFAULT_LOG,
                    "ENTRY_CACHE_SIZE({}) is below the recommended minimum "
                    "of 20000",
                    mConfig.ENTRY_CACHE_SIZE);
    }
    mLedgerTxnRoot = std::make_unique<LedgerTxnRoot>(
        *this, mConfig.ENTRY_CACHE_SIZE, mConfig.PREFETCH_BATCH_SIZE
#ifdef BEST_OFFER_DEBUGGING
        ,
        mConfig.BEST_OFFER_DEBUGGING_ENABLED
#endif
    );

#ifdef BUILD_TESTS
    if (getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        resetLedgerState();
    }
#endif

    BucketListIsConsistentWithDatabase::registerInvariant(*this);
    AccountSubEntriesCountIsValid::registerInvariant(*this);
    ConservationOfLumens::registerInvariant(*this);
    LedgerEntryIsValid::registerInvariant(*this);
    LiabilitiesMatchOffers::registerInvariant(*this);
    SponsorshipCountIsValid::registerInvariant(*this);
    ConstantProductInvariant::registerInvariant(*this);
    enableInvariantsFromConfig();

    if (initNewDB)
    {
        newDB();
    }
    else
    {
        upgradeToCurrentSchemaAndMaybeRebuildLedger(true, forceRebuild);
    }

    // Subtle: process manager should come to existence _after_ BucketManager
    // initialization and newDB run, as it relies on tmp dir created in the
    // constructor
    mProcessManager = ProcessManager::create(*this);

    // After everything is initialized, start accepting HTTP commands
    mCommandHandler = std::make_unique<CommandHandler>(*this);

    LOG_DEBUG(DEFAULT_LOG, "Application constructed");
}

void
ApplicationImpl::resetLedgerState()
{
#ifdef BUILD_TESTS
    if (getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        mNeverCommittingLedgerTxn.reset();
        mInMemoryLedgerTxnRoot = std::make_unique<InMemoryLedgerTxnRoot>(
#ifdef BEST_OFFER_DEBUGGING
            mConfig.BEST_OFFER_DEBUGGING_ENABLED
#endif
        );
        mNeverCommittingLedgerTxn = std::make_unique<InMemoryLedgerTxn>(
            *mInMemoryLedgerTxnRoot, getDatabase(), *mLedgerTxnRoot);
    }
    else
#endif
    {
        auto& lsRoot = getLedgerTxnRoot();
        lsRoot.deleteOffersModifiedOnOrAfterLedger(0);
    }
}

void
ApplicationImpl::newDB()
{
    mDatabase->initialize();
    upgradeToCurrentSchemaAndMaybeRebuildLedger(false, true);
    mLedgerManager->startNewLedger();
}

void
ApplicationImpl::upgradeToCurrentSchemaAndMaybeRebuildLedger(bool applyBuckets,
                                                             bool forceRebuild)
{
    if (forceRebuild)
    {
        auto& ps = getPersistentState();
        ps.setRebuildForOfferTable();
    }

    mDatabase->upgradeToCurrentSchema();
    maybeRebuildLedger(*this, applyBuckets);
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
ApplicationImpl::getJsonInfo(bool verbose)
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
    if (lm.hasSorobanNetworkConfig())
    {
        info["ledger"]["maxSorobanTxSetSize"] =
            static_cast<Json::Int64>(lm.maxLedgerResources(/* isSoroban */ true)
                                         .getVal(Resource::Type::OPERATIONS));
    }

    auto currentHeaderFlags = LedgerHeaderUtils::getFlags(lcl.header);
    if (currentHeaderFlags != 0)
    {
        info["ledger"]["flags"] = currentHeaderFlags;
    }

    info["ledger"]["age"] = (int)lm.secondsSinceLastLedgerClose();

    if (verbose)
    {
        auto has = lm.getLastClosedLedgerHAS();
        auto& levels = info["ledger"]["bucketlist"];
        for (auto const& l : has.currentBuckets)
        {
            Json::Value levelInfo;
            levelInfo["curr"] = l.curr;
            levelInfo["snap"] = l.snap;
            levels.append(levelInfo);
        }
    }

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
    auto ledgerSeq = lcl.header.ledgerSeq;
    if (herder.getState() != Herder::HERDER_BOOTING_STATE)
    {
        ledgerSeq = herder.trackingConsensusLedgerIndex();
    }

    if (ledgerSeq > 1)
    {
        quorumInfo = herder.getJsonQuorumInfo(
            getConfig().NODE_SEED.getPublicKey(), true, false, ledgerSeq - 1);
    }

    // If the quorum set info for the previous ledger is missing, use the
    // current ledger
    auto qset = quorumInfo.get("qset", "");
    if (quorumInfo.empty() || qset.empty())
    {
        quorumInfo = herder.getJsonQuorumInfo(
            getConfig().NODE_SEED.getPublicKey(), true, false, ledgerSeq);
    }

    auto invariantFailures = getInvariantManager().getJsonInfo();
    if (!invariantFailures.empty())
    {
        info["invariant_failures"] = invariantFailures;
    }

    return root;
}

void
ApplicationImpl::reportInfo(bool verbose)
{
    mLedgerManager->loadLastKnownLedger(/* restoreBucketlist */ false);
    LOG_INFO(DEFAULT_LOG, "Reporting application info");
    std::cout << getJsonInfo(verbose).toStyledString() << std::endl;
}

std::shared_ptr<BasicWork>
ApplicationImpl::scheduleSelfCheck(bool waitUntilNextCheckpoint)
{
    // Ensure the timer is re-triggered no matter what.
    if (mConfig.AUTOMATIC_SELF_CHECK_PERIOD.count() != 0)
    {
        mSelfCheckTimer.expires_from_now(mConfig.AUTOMATIC_SELF_CHECK_PERIOD);
        mSelfCheckTimer.async_wait(
            [this, waitUntilNextCheckpoint]() {
                scheduleSelfCheck(waitUntilNextCheckpoint);
            },
            VirtualTimer::onFailureNoop);
    }

    // We store any self-check that gets scheduled so that a second call to
    // schedule a self-check while one is running returns the running one.
    if (auto existing = mRunningSelfCheck.lock())
    {
        return existing;
    }
    auto& ws = getWorkScheduler();
    auto& lm = getLedgerManager();
    auto& ham = getHistoryArchiveManager();

    LedgerHeaderHistoryEntry lhhe = lm.getLastClosedLedgerHeader();

    std::vector<std::shared_ptr<BasicWork>> seq;
    seq.emplace_back(ham.getHistoryArchiveReportWork());

    auto checkLedger = ham.getCheckLedgerHeaderWork(lhhe);
    if (waitUntilNextCheckpoint)
    {
        // Delay until a second full checkpoint-period after the next checkpoint
        // publication. The captured lhhe should usually be published by then.
        auto targetLedger =
            HistoryManager::firstLedgerAfterCheckpointContaining(
                lhhe.header.ledgerSeq, getConfig());
        targetLedger = HistoryManager::firstLedgerAfterCheckpointContaining(
            targetLedger, getConfig());
        auto cond = [targetLedger](Application& app) -> bool {
            auto& lm = app.getLedgerManager();
            return lm.getLastClosedLedgerNum() > targetLedger;
        };
        seq.emplace_back(std::make_shared<ConditionalWork>(
            *this, "wait-for-lcl", cond, checkLedger,
            std::chrono::seconds(10)));
    }
    else
    {
        seq.emplace_back(checkLedger);
    }

    // Stash a weak ptr to the shared ptr we're returning, for subsequent calls.
    std::shared_ptr<BasicWork> ptr =
        ws.scheduleWork<WorkSequence>("self-check", seq, BasicWork::RETRY_NEVER,
                                      /*stopOnFirstFailure=*/false);
    mRunningSelfCheck = ptr;

    return ptr;
}

Hash const&
ApplicationImpl::getNetworkID() const
{
    return mNetworkID;
}

ApplicationImpl::~ApplicationImpl()
{
    LOG_INFO(DEFAULT_LOG, "Application destructing");
    mStopping = true;
    try
    {
        // First, shutdown ledger close queue _before_ shutting down all the
        // subsystems. This ensures that any ledger currently being closed
        // finishes okay
        shutdownLedgerCloseThread();
        shutdownWorkScheduler();
        if (mProcessManager)
        {
            mProcessManager->shutdown();
        }
        if (mBucketManager)
        {
            mBucketManager->shutdown();
        }
        // Peers continue reading and writing in the background, so we need to
        // issue a signal to start wrapping up
        if (mOverlayManager)
        {
            mOverlayManager->shutdown();
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
ApplicationImpl::validateAndLogConfig()
{
    if (mConfig.FORCE_SCP && !mConfig.NODE_IS_VALIDATOR)
    {
        throw std::invalid_argument(
            "FORCE_SCP is set but NODE_IS_VALIDATOR not set");
    }

    auto const isNetworkedValidator =
        mConfig.NODE_IS_VALIDATOR && !mConfig.RUN_STANDALONE;

    if (mConfig.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS && isNetworkedValidator)
    {
        throw std::invalid_argument("ENABLE_SOROBAN_DIAGNOSTIC_EVENTS is set, "
                                    "NODE_IS_VALIDATOR is set, and "
                                    "RUN_STANDALONE is not set");
    }

    if (mConfig.METADATA_OUTPUT_STREAM != "" && isNetworkedValidator)
    {
        throw std::invalid_argument(
            "METADATA_OUTPUT_STREAM is set, NODE_IS_VALIDATOR is set, and "
            "RUN_STANDALONE is not set");
    }

    if (!mDatabase->isSqlite())
    {
        CLOG_WARNING(Database,
                     "Non-sqlite3 database detected. Support for other sql "
                     "backends is deprecated and will be removed in a future "
                     "release. Please use sqlite3 for non-ledger state data.");
    }

    auto pageSizeExp = mConfig.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;

    // If the page size is less than 256 bytes, it is essentially
    // indexing individual keys, so page size should be set to 0
    // instead.
    static auto const pageSizeMinExponent = 8;

    // Any exponent above 31 will cause overflow
    static auto const pageSizeMaxExponent = 31;

    if (pageSizeExp != 0)
    {
        if (pageSizeExp < pageSizeMinExponent)
        {
            throw std::invalid_argument(
                "BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT "
                "must be at least 8 or set to 0 for individual entry "
                "indexing");
        }

        if (pageSizeExp > pageSizeMaxExponent)
        {
            throw std::invalid_argument(
                "BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT "
                "must be less than 32");
        }
    }

    CLOG_INFO(Bucket,
              "BucketListDB enabled: pageSizeExponent: {} indexCutOff: "
              "{}MB, persist indexes: {}",
              pageSizeExp, mConfig.BUCKETLIST_DB_INDEX_CUTOFF,
              mConfig.BUCKETLIST_DB_PERSIST_INDEX);

    if (mConfig.HTTP_QUERY_PORT != 0)
    {
        if (isNetworkedValidator)
        {
            throw std::invalid_argument("HTTP_QUERY_PORT is non-zero, "
                                        "NODE_IS_VALIDATOR is set, and "
                                        "RUN_STANDALONE is not set");
        }

        if (mConfig.HTTP_QUERY_PORT == mConfig.HTTP_PORT)
        {
            throw std::invalid_argument(
                "HTTP_QUERY_PORT must be different from HTTP_PORT");
        }

        if (mConfig.QUERY_THREAD_POOL_SIZE == 0)
        {
            throw std::invalid_argument(
                "HTTP_QUERY_PORT requires QUERY_THREAD_POOL_SIZE > 0");
        }
    }

    if (getHistoryArchiveManager().publishEnabled())
    {
        if (!mConfig.modeStoresAllHistory())
        {
            throw std::invalid_argument(
                "Core is not configured to store history, but "
                "some history archives are writable");
        }
    }

    if (mConfig.QUORUM_SET.threshold == 0)
    {
        throw std::invalid_argument("Quorum not configured");
    }

    mConfig.logBasicInfo();
}

void
ApplicationImpl::startServices()
{
    // restores Herder's state before starting overlay
    mHerder->start();
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
    if (mConfig.AUTOMATIC_SELF_CHECK_PERIOD.count() != 0)
    {
        scheduleSelfCheck(true);
    }

    if (mConfig.TESTING_UPGRADE_DATETIME.time_since_epoch().count() != 0)
    {
        mHerder->setUpgrades(mConfig);
    }
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

    mLedgerManager->loadLastKnownLedger(/* restoreBucketlist */ true);
    startServices();
}

void
ApplicationImpl::gracefulStop()
{
    if (mStopping)
    {
        return;
    }
    mStopping = true;
    shutdownLedgerCloseThread();
    if (mOverlayManager)
    {
        mOverlayManager->shutdown();
    }
    mSelfCheckTimer.cancel();
    shutdownWorkScheduler();
    if (mProcessManager)
    {
        mProcessManager->shutdown();
    }
    if (mBucketManager)
    {
        // This call happens in shutdown -- before destruction -- so that we can
        // be sure other subsystems (ledger etc.) are still alive and we can
        // call into them to figure out which buckets _are_ referenced.
        mBucketManager->forgetUnreferencedBuckets(
            mLedgerManager->getLastClosedLedgerHAS());
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
    mVirtualClock.shutdown();
}

void
ApplicationImpl::shutdownWorkScheduler()
{
    if (mWorkScheduler)
    {
        mWorkScheduler->shutdown();
    }
}

void
ApplicationImpl::shutdownLedgerCloseThread()
{
    if (mLedgerCloseThread && !mLedgerCloseThreadStopped)
    {
        if (mLedgerCloseWork)
        {
            mLedgerCloseWork.reset();
        }
        LOG_INFO(DEFAULT_LOG, "Joining the ledger close thread");
        mLedgerCloseThread->join();
        mLedgerCloseThreadStopped = true;
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
    if (mOverlayWork)
    {
        mOverlayWork.reset();
    }
    if (mEvictionWork)
    {
        mEvictionWork.reset();
    }

    LOG_INFO(DEFAULT_LOG, "Joining {} worker threads", mWorkerThreads.size());
    for (auto& w : mWorkerThreads)
    {
        w.join();
    }

    if (mOverlayThread)
    {
        LOG_INFO(DEFAULT_LOG, "Joining the overlay thread");
        mOverlayThread->join();
    }

    if (mEvictionThread)
    {
        LOG_INFO(DEFAULT_LOG, "Joining eviction thread");
        mEvictionThread->join();
    }

    LOG_INFO(DEFAULT_LOG, "Joined all {} threads", (mWorkerThreads.size() + 1));
}

std::string
ApplicationImpl::manualClose(std::optional<uint32_t> const& manualLedgerSeq,
                             std::optional<TimePoint> const& manualCloseTime)
{
    releaseAssert(threadIsMain());

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
                        "sequence number increment (expected {:d}; got {:d})"),
                    targetLedgerSeq, newLedgerSeq));
            }

            return fmt::format(
                FMT_STRING("Manually closed ledger with sequence "
                           "number {:d} and closeTime {:d}"),
                targetLedgerSeq,
                getLedgerManager()
                    .getLastClosedLedgerHeader()
                    .header.scpValue.closeTime);
        }

        return fmt::format(
            FMT_STRING("Manually triggered a ledger close with sequence "
                       "number {:d}"),
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
    std::optional<uint32_t> const& explicitlyProvidedSeqNum)
{
    auto const startLedgerSeq = getLedgerManager().getLastClosedLedgerNum();
    // -1: after externalizing we want to make sure that we don't reason about
    // ledgers that overflow int32
    auto const maxLedgerSeq =
        static_cast<uint32>(std::numeric_limits<int32_t>::max() - 1);

    // The "scphistory" stores ledger sequence numbers as INTs.
    if (startLedgerSeq >= maxLedgerSeq)
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("Manually closed ledger sequence number "
                                   "({:d}) already at max ({:d})"),
                        startLedgerSeq, maxLedgerSeq));
    }

    auto const nextLedgerSeq = startLedgerSeq + 1;

    if (explicitlyProvidedSeqNum)
    {
        if (*explicitlyProvidedSeqNum > maxLedgerSeq)
        {
            // The "scphistory" stores ledger sequence numbers as INTs.
            throw std::invalid_argument(
                fmt::format(FMT_STRING("Manual close ledger sequence number "
                                       "{:d} beyond max ({:d})"),
                            *explicitlyProvidedSeqNum, maxLedgerSeq));
        }

        if (*explicitlyProvidedSeqNum <= startLedgerSeq)
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("Invalid manual close ledger sequence number {:d} "
                           "(must exceed current sequence number {:d})"),
                *explicitlyProvidedSeqNum, startLedgerSeq));
        }
    }

    return explicitlyProvidedSeqNum ? *explicitlyProvidedSeqNum : nextLedgerSeq;
}

void
ApplicationImpl::setManualCloseVirtualTime(
    std::optional<TimePoint> const& explicitlyProvidedCloseTime)
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
                fmt::format(FMT_STRING("Manual close time {:d} too early (last "
                                       "close time={:d}, now={:d})"),
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
        throw std::invalid_argument(fmt::format(
            FMT_STRING("New close time {:d} would exceed max ({:d})"),
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
ApplicationImpl::generateLoad(GeneratedLoadConfig cfg)
{
    getMetrics().NewMeter({"loadgen", "run", "start"}, "run").Mark();
    getLoadGenerator().generateLoad(cfg);
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
Config&
ApplicationImpl::getMutableConfig()
{
    return mConfig;
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

    if (!mStarted || mHerder->getState() == Herder::HERDER_BOOTING_STATE)
    {
        s = APP_CREATED_STATE;
    }
    else if (mStopping)
    {
        s = APP_STOPPING_STATE;
    }
    else if (mHerder->getState() != Herder::HERDER_TRACKING_NETWORK_STATE)
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
    static std::array<const char*, APP_NUM_STATE> stateStrings =
        std::array{"Booting",     "Joining SCP", "Connected",
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

    // Update overlay inbound-connections and file-handle metrics.
    if (mOverlayManager)
    {
        mMetrics->NewCounter({"overlay", "inbound", "live"})
            .set_count(*mOverlayManager->getLiveInboundPeersCounter());
    }
    mMetrics->NewCounter({"process", "file", "handles"})
        .set_count(fs::getOpenHandleCount());
}

void
ApplicationImpl::syncAllMetrics()
{
    mHerder->syncMetrics();
    mLedgerManager->syncMetrics();
    mLedgerApplyManager->syncMetrics();
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

LedgerApplyManager&
ApplicationImpl::getLedgerApplyManager()
{
    return *mLedgerApplyManager;
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

asio::io_context&
ApplicationImpl::getEvictionIOContext()
{
    releaseAssert(mEvictionIOContext);
    return *mEvictionIOContext;
}

asio::io_context&
ApplicationImpl::getOverlayIOContext()
{
    releaseAssert(mOverlayIOContext);
    return *mOverlayIOContext;
}

asio::io_context&
ApplicationImpl::getLedgerCloseIOContext()
{
    releaseAssert(mLedgerCloseIOContext);
    return *mLedgerCloseIOContext;
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
            auto sleepFor =
                this->getConfig().ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING;
            if (sleepFor > std::chrono::microseconds::zero())
            {
                std::this_thread::sleep_for(sleepFor);
            }
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
ApplicationImpl::postOnEvictionBackgroundThread(std::function<void()>&& f,
                                                std::string jobName)
{
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    asio::post(getEvictionIOContext(), [this, f = std::move(f), isSlow]() {
        mPostOnBackgroundThreadDelay.Update(isSlow.checkElapsedTime());
        f();
    });
}

void
ApplicationImpl::postOnOverlayThread(std::function<void()>&& f,
                                     std::string jobName)
{
    releaseAssert(mOverlayIOContext);
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    asio::post(*mOverlayIOContext, [this, f = std::move(f), isSlow]() {
        mPostOnOverlayThreadDelay.Update(isSlow.checkElapsedTime());
        f();
    });
}

void
ApplicationImpl::postOnLedgerCloseThread(std::function<void()>&& f,
                                         std::string jobName)
{
    releaseAssert(mLedgerCloseIOContext);
    LogSlowExecution isSlow{std::move(jobName), LogSlowExecution::Mode::MANUAL,
                            "executed after"};
    asio::post(*mLedgerCloseIOContext, [this, f = std::move(f), isSlow]() {
        mPostOnLedgerCloseThreadDelay.Update(isSlow.checkElapsedTime());
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
#ifdef BUILD_TESTS
    if (mConfig.MODE_USES_IN_MEMORY_LEDGER)
    {
        return *mNeverCommittingLedgerTxn;
    }
#endif

    return *mLedgerTxnRoot;
}

AppConnector&
ApplicationImpl::getAppConnector()
{
    return *mAppConnector;
}
}
