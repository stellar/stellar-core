// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManagerImpl.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "catchup/AssumeStateWork.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/HerderPersistence.h"
#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "herder/Upgrades.h"
#include "history/HistoryManager.h"
#include "ledger/FlushAndRotateMetaDebugWork.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMetaFrame.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "util/DebugMetaUtils.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include "util/XDRStream.h"
#include "work/WorkScheduler.h"
#include "xdrpp/printer.h"

#include <fmt/format.h>

#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/types.h"

#include "medida/buckets.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include <Tracy.hpp>

#include <chrono>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

/*
The ledger module:
    1) gets the externalized tx set
    2) applies this set to the last closed ledger
    3) sends the changed entries to the BucketList
    4) saves the changed entries to SQL
    5) saves the ledger hash and header to SQL
    6) sends the new ledger hash and the tx set to the history
    7) sends the new ledger hash and header to the Herder


catching up to network:
    1) Wait for SCP to tell us what the network is on now
    2) Pull history log or static deltas from history archive
    3) Replay or force-apply deltas, depending on catchup mode

*/
using namespace std;

namespace stellar
{

const uint32_t LedgerManager::GENESIS_LEDGER_SEQ = 1;
const uint32_t LedgerManager::GENESIS_LEDGER_VERSION = 0;
const uint32_t LedgerManager::GENESIS_LEDGER_BASE_FEE = 100;
const uint32_t LedgerManager::GENESIS_LEDGER_BASE_RESERVE = 100000000;
const uint32_t LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE = 100;
const int64_t LedgerManager::GENESIS_LEDGER_TOTAL_COINS = 1000000000000000000;

std::unique_ptr<LedgerManager>
LedgerManager::create(Application& app)
{
    return std::make_unique<LedgerManagerImpl>(app);
}

std::string
LedgerManager::ledgerAbbrev(LedgerHeader const& header)
{
    return ledgerAbbrev(header, xdrSha256(header));
}

std::string
LedgerManager::ledgerAbbrev(uint32_t seq, uint256 const& hash)
{
    std::ostringstream oss;
    oss << "[seq=" << seq << ", hash=" << hexAbbrev(hash) << "]";
    return oss.str();
}

std::string
LedgerManager::ledgerAbbrev(LedgerHeader const& header, uint256 const& hash)
{
    return ledgerAbbrev(header.ledgerSeq, hash);
}

std::string
LedgerManager::ledgerAbbrev(LedgerHeaderHistoryEntry const& he)
{
    return ledgerAbbrev(he.header, he.hash);
}

LedgerManagerImpl::LedgerManagerImpl(Application& app)
    : mApp(app)
    , mSorobanMetrics(app.getMetrics())
    , mTransactionApply(
          app.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mTransactionCount(
          app.getMetrics().NewHistogram({"ledger", "transaction", "count"}))
    , mOperationCount(
          app.getMetrics().NewHistogram({"ledger", "operation", "count"}))
    , mPrefetchHitRate(
          app.getMetrics().NewHistogram({"ledger", "prefetch", "hit-rate"}))
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
    , mLedgerAgeClosed(app.getMetrics().NewBuckets(
          {"ledger", "age", "closed"}, {5000.0, 7000.0, 10000.0, 20000.0}))
    , mLedgerAge(
          app.getMetrics().NewCounter({"ledger", "age", "current-seconds"}))
    , mTransactionApplySucceeded(
          app.getMetrics().NewCounter({"ledger", "apply", "success"}))
    , mTransactionApplyFailed(
          app.getMetrics().NewCounter({"ledger", "apply", "failure"}))
    , mSorobanTransactionApplySucceeded(
          app.getMetrics().NewCounter({"ledger", "apply-soroban", "success"}))
    , mSorobanTransactionApplyFailed(
          app.getMetrics().NewCounter({"ledger", "apply-soroban", "failure"}))
    , mMetaStreamBytes(
          app.getMetrics().NewMeter({"ledger", "metastream", "bytes"}, "byte"))
    , mMetaStreamWriteTime(
          app.getMetrics().NewTimer({"ledger", "metastream", "write"}))
    , mLastClose(mApp.getClock().now())
    , mCatchupDuration(
          app.getMetrics().NewTimer({"ledger", "catchup", "duration"}))
    , mState(LM_BOOTING_STATE)

{
    setupLedgerCloseMetaStream();
}

void
LedgerManagerImpl::moveToSynced()
{
    setState(LM_SYNCED_STATE);
}

void
LedgerManagerImpl::beginApply()
{
    // Go into "applying" state, this will prevent catchup from starting
    mCurrentlyApplyingLedger = true;

    // Notify Herder that application started, so it won't fire out of sync
    // timer
    mApp.getHerder().beginApply();
}

void
LedgerManagerImpl::setState(State s)
{
    releaseAssert(threadIsMain());
    if (s != getState())
    {
        std::string oldState = getStateHuman();
        mState = s;
        mApp.syncOwnMetrics();
        CLOG_INFO(Ledger, "Changing state {} -> {}", oldState, getStateHuman());
        if (mState != LM_CATCHING_UP_STATE)
        {
            mApp.getLedgerApplyManager().logAndUpdateCatchupStatus(true);
        }
    }
}

LedgerManager::State
LedgerManagerImpl::getState() const
{
    return mState;
}

std::string
LedgerManagerImpl::getStateHuman() const
{
    static std::array<const char*, LM_NUM_STATE> stateStrings = std::array{
        "LM_BOOTING_STATE", "LM_SYNCED_STATE", "LM_CATCHING_UP_STATE"};
    return std::string(stateStrings[getState()]);
}

LedgerHeader
LedgerManager::genesisLedger()
{
    LedgerHeader result;
    // all fields are initialized by default to 0
    // set the ones that are not 0
    result.ledgerVersion = GENESIS_LEDGER_VERSION;
    result.baseFee = GENESIS_LEDGER_BASE_FEE;
    result.baseReserve = GENESIS_LEDGER_BASE_RESERVE;
    result.maxTxSetSize = GENESIS_LEDGER_MAX_TX_SIZE;
    result.totalCoins = GENESIS_LEDGER_TOTAL_COINS;
    result.ledgerSeq = GENESIS_LEDGER_SEQ;
    return result;
}

void
LedgerManagerImpl::startNewLedger(LedgerHeader const& genesisLedger)
{
    auto ledgerTime = mLedgerClose.TimeScope();
    SecretKey skey = SecretKey::fromSeed(mApp.getNetworkID());

    LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
    auto const& cfg = mApp.getConfig();

    ltx.loadHeader().current() = genesisLedger;
    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        SorobanNetworkConfig::initializeGenesisLedgerForTesting(
            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION, ltx, mApp);
    }

    LedgerEntry rootEntry;
    rootEntry.lastModifiedLedgerSeq = 1;
    rootEntry.data.type(ACCOUNT);
    auto& rootAccount = rootEntry.data.account();
    rootAccount.accountID = skey.getPublicKey();
    rootAccount.thresholds[0] = 1;
    rootAccount.balance = genesisLedger.totalCoins;
    ltx.create(rootEntry);

    CLOG_INFO(Ledger, "Established genesis ledger, closing");
    CLOG_INFO(Ledger, "Root account: {}", skey.getStrKeyPublic());
    CLOG_INFO(Ledger, "Root account seed: {}", skey.getStrKeySeed().value);
    auto output =
        ledgerClosed(ltx, /*ledgerCloseMeta*/ nullptr, /*initialLedgerVers*/ 0);
    advanceLedgerPointers(output);

    ltx.commit();
}

void
LedgerManagerImpl::startNewLedger()
{
    auto ledger = genesisLedger();
    auto const& cfg = mApp.getConfig();
    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        ledger.ledgerVersion = cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION;
        ledger.baseFee = cfg.TESTING_UPGRADE_DESIRED_FEE;
        ledger.baseReserve = cfg.TESTING_UPGRADE_RESERVE;
        ledger.maxTxSetSize = cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE;
    }

    startNewLedger(ledger);
}

static void
setLedgerTxnHeader(LedgerHeader const& lh, Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    ltx.loadHeader().current() = lh;
    ltx.commit();
}

void
LedgerManagerImpl::loadLastKnownLedger(bool restoreBucketlist)
{
    ZoneScoped;

    // Step 1. Load LCL state from the DB and extract latest ledger hash
    string lastLedger = mApp.getPersistentState().getState(
        PersistentState::kLastClosedLedger, mApp.getDatabase().getSession());

    if (lastLedger.empty())
    {
        throw std::runtime_error(
            "No reference in DB to any last closed ledger");
    }

    CLOG_INFO(Ledger, "Last closed ledger (LCL) hash is {}", lastLedger);
    Hash lastLedgerHash = hexToBin256(lastLedger);

    HistoryArchiveState has;
    has.fromString(mApp.getPersistentState().getState(
        PersistentState::kHistoryArchiveState,
        mApp.getDatabase().getSession()));

    // Step 2. Restore LedgerHeader from DB based on the ledger hash derived
    // earlier, or verify we're at genesis if in no-history mode
    std::optional<LedgerHeader> latestLedgerHeader;
    if (mApp.getConfig().MODE_STORES_HISTORY_LEDGERHEADERS)
    {
        if (mRebuildInMemoryState)
        {
            LedgerHeader lh;
            CLOG_INFO(Ledger,
                      "Setting empty ledger while core rebuilds state: {}",
                      ledgerAbbrev(lh));
            setLedgerTxnHeader(lh, mApp);
            latestLedgerHeader = lh;
        }
        else
        {
            auto currentLedger =
                LedgerHeaderUtils::loadByHash(getDatabase(), lastLedgerHash);
            if (!currentLedger)
            {
                throw std::runtime_error("Could not load ledger from database");
            }

            if (currentLedger->ledgerSeq != has.currentLedger)
            {
                throw std::runtime_error("Invalid database state: last known "
                                         "ledger does not agree with HAS");
            }

            CLOG_INFO(Ledger, "Loaded LCL header from database: {}",
                      ledgerAbbrev(*currentLedger));
            setLedgerTxnHeader(*currentLedger, mApp);
            latestLedgerHeader = *currentLedger;
        }
    }
    else
    {
        // In no-history mode, this method should only be called when
        // the LCL is genesis.
        releaseAssertOrThrow(mLastClosedLedger.hash == lastLedgerHash);
        releaseAssertOrThrow(mLastClosedLedger.header.ledgerSeq ==
                             GENESIS_LEDGER_SEQ);
        CLOG_INFO(Ledger, "LCL is genesis: {}",
                  ledgerAbbrev(mLastClosedLedger));
        latestLedgerHeader = mLastClosedLedger.header;
    }

    releaseAssert(latestLedgerHeader.has_value());

    auto missing = mApp.getBucketManager().checkForMissingBucketsFiles(has);
    auto pubmissing =
        mApp.getHistoryManager().getMissingBucketsReferencedByPublishQueue();
    missing.insert(missing.end(), pubmissing.begin(), pubmissing.end());
    if (!missing.empty())
    {
        CLOG_ERROR(Ledger, "{} buckets are missing from bucket directory '{}'",
                   missing.size(), mApp.getBucketManager().getBucketDir());
        throw std::runtime_error("Bucket directory is corrupt");
    }

    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        // Only restart merges in full startup mode. Many modes in core
        // (standalone offline commands, in-memory setup) do not need to
        // spin up expensive merge processes.
        auto assumeStateWork =
            mApp.getWorkScheduler().executeWork<AssumeStateWork>(
                has, latestLedgerHeader->ledgerVersion, restoreBucketlist);
        if (assumeStateWork->getState() == BasicWork::State::WORK_SUCCESS)
        {
            CLOG_INFO(Ledger, "Assumed bucket-state for LCL: {}",
                      ledgerAbbrev(*latestLedgerHeader));
        }
        else
        {
            // Work should only fail during graceful shutdown
            releaseAssertOrThrow(mApp.isStopping());
        }
    }

    // Step 4. Restore LedgerManager's internal state
    auto output = advanceLedgerStateSnapshot(*latestLedgerHeader, has);
    advanceLedgerPointers(output);

    // Maybe truncate checkpoint files if we're restarting after a crash
    // in closeLedger (in which case any modifications to the ledger state have
    // been rolled back)
    mApp.getHistoryManager().restoreCheckpoint(latestLedgerHeader->ledgerSeq);

    if (protocolVersionStartsFrom(latestLedgerHeader->ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        // Step 5. If ledger state is ready and core is in v20, load network
        // configs right away
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        updateNetworkConfig(ltx);
        mSorobanNetworkConfigReadOnly = mSorobanNetworkConfigForApply;
    }
}

Database&
LedgerManagerImpl::getDatabase()
{
    return mApp.getDatabase();
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSize() const
{
    releaseAssert(threadIsMain());
    return mLastClosedLedger.header.maxTxSetSize;
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSizeOps() const
{
    releaseAssert(threadIsMain());
    auto n = mLastClosedLedger.header.maxTxSetSize;
    return protocolVersionStartsFrom(mLastClosedLedger.header.ledgerVersion,
                                     ProtocolVersion::V_11)
               ? n
               : (n * MAX_OPS_PER_TX);
}

Resource
LedgerManagerImpl::maxLedgerResources(bool isSoroban)
{
    ZoneScoped;

    if (isSoroban)
    {
        return getSorobanNetworkConfigReadOnly().maxLedgerResources();
    }
    else
    {
        uint32_t maxOpsLedger = getLastMaxTxSetSizeOps();
        return Resource(maxOpsLedger);
    }
}

Resource
LedgerManagerImpl::maxSorobanTransactionResources()
{
    ZoneScoped;

    auto const& conf =
        mApp.getLedgerManager().getSorobanNetworkConfigReadOnly();
    int64_t const opCount = 1;
    std::vector<int64_t> limits = {opCount,
                                   conf.txMaxInstructions(),
                                   conf.txMaxSizeBytes(),
                                   conf.txMaxReadBytes(),
                                   conf.txMaxWriteBytes(),
                                   conf.txMaxReadLedgerEntries(),
                                   conf.txMaxWriteLedgerEntries()};
    return Resource(limits);
}

int64_t
LedgerManagerImpl::getLastMinBalance(uint32_t ownerCount) const
{
    releaseAssert(threadIsMain());
    auto const& lh = mLastClosedLedger.header;
    if (protocolVersionIsBefore(lh.ledgerVersion, ProtocolVersion::V_9))
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2LL + ownerCount) * int64_t(lh.baseReserve);
}

uint32_t
LedgerManagerImpl::getLastReserve() const
{
    releaseAssert(threadIsMain());
    return mLastClosedLedger.header.baseReserve;
}

uint32_t
LedgerManagerImpl::getLastTxFee() const
{
    releaseAssert(threadIsMain());
    return mLastClosedLedger.header.baseFee;
}

LedgerHeaderHistoryEntry const&
LedgerManagerImpl::getLastClosedLedgerHeader() const
{
    releaseAssert(threadIsMain());
    return mLastClosedLedger;
}

HistoryArchiveState
LedgerManagerImpl::getLastClosedLedgerHAS()
{
    releaseAssert(threadIsMain());
    return mLastClosedLedgerHAS;
}

uint32_t
LedgerManagerImpl::getLastClosedLedgerNum() const
{
    releaseAssert(threadIsMain());
    return mLastClosedLedger.header.ledgerSeq;
}

SorobanNetworkConfig const&
LedgerManagerImpl::getSorobanNetworkConfigReadOnly()
{
    releaseAssert(threadIsMain());
    releaseAssert(hasSorobanNetworkConfig());
    return *mSorobanNetworkConfigReadOnly;
}

SorobanNetworkConfig const&
LedgerManagerImpl::getSorobanNetworkConfigForApply()
{
    releaseAssert(mSorobanNetworkConfigForApply);
    return *mSorobanNetworkConfigForApply;
}

bool
LedgerManagerImpl::hasSorobanNetworkConfig() const
{
    releaseAssert(threadIsMain());
    return static_cast<bool>(mSorobanNetworkConfigReadOnly);
}

#ifdef BUILD_TESTS
SorobanNetworkConfig&
LedgerManagerImpl::getMutableSorobanNetworkConfig()
{
    releaseAssert(threadIsMain());
    return *mSorobanNetworkConfigForApply;
}

std::vector<TransactionMetaFrame> const&
LedgerManagerImpl::getLastClosedLedgerTxMeta()
{
    return mLastLedgerTxMeta;
}

void
LedgerManagerImpl::storeCurrentLedgerForTest(LedgerHeader const& header)
{
    storeCurrentLedger(header, true, true);
}
#endif

SorobanMetrics&
LedgerManagerImpl::getSorobanMetrics()
{
    return mSorobanMetrics;
}

void
LedgerManagerImpl::publishSorobanMetrics()
{
    auto const& conf = getSorobanNetworkConfigForApply();
    // first publish the network config limits
    mSorobanMetrics.mConfigContractDataKeySizeBytes.set_count(
        conf.maxContractDataKeySizeBytes());
    mSorobanMetrics.mConfigMaxContractDataEntrySizeBytes.set_count(
        conf.maxContractDataEntrySizeBytes());
    mSorobanMetrics.mConfigMaxContractSizeBytes.set_count(
        conf.maxContractSizeBytes());
    mSorobanMetrics.mConfigTxMaxSizeByte.set_count(conf.txMaxSizeBytes());
    mSorobanMetrics.mConfigTxMaxCpuInsn.set_count(conf.txMaxInstructions());
    mSorobanMetrics.mConfigTxMemoryLimitBytes.set_count(conf.txMemoryLimit());
    mSorobanMetrics.mConfigTxMaxReadLedgerEntries.set_count(
        conf.txMaxReadLedgerEntries());
    mSorobanMetrics.mConfigTxMaxReadBytes.set_count(conf.txMaxReadBytes());
    mSorobanMetrics.mConfigTxMaxWriteLedgerEntries.set_count(
        conf.txMaxWriteLedgerEntries());
    mSorobanMetrics.mConfigTxMaxWriteBytes.set_count(conf.txMaxWriteBytes());
    mSorobanMetrics.mConfigMaxContractEventsSizeBytes.set_count(
        conf.txMaxContractEventsSizeBytes());
    mSorobanMetrics.mConfigLedgerMaxTxCount.set_count(conf.ledgerMaxTxCount());
    mSorobanMetrics.mConfigLedgerMaxInstructions.set_count(
        conf.ledgerMaxInstructions());
    mSorobanMetrics.mConfigLedgerMaxTxsSizeByte.set_count(
        conf.ledgerMaxTransactionSizesBytes());
    mSorobanMetrics.mConfigLedgerMaxReadLedgerEntries.set_count(
        conf.ledgerMaxReadLedgerEntries());
    mSorobanMetrics.mConfigLedgerMaxReadBytes.set_count(
        conf.ledgerMaxReadBytes());
    mSorobanMetrics.mConfigLedgerMaxWriteEntries.set_count(
        conf.ledgerMaxWriteLedgerEntries());
    mSorobanMetrics.mConfigLedgerMaxWriteBytes.set_count(
        conf.ledgerMaxWriteBytes());
    mSorobanMetrics.mConfigBucketListTargetSizeByte.set_count(
        conf.bucketListTargetSizeBytes());
    mSorobanMetrics.mConfigFeeWrite1KB.set_count(conf.feeWrite1KB());

    // then publish the actual ledger usage
    mSorobanMetrics.publishAndResetLedgerWideMetrics();
}

// called by txherder
void
LedgerManagerImpl::valueExternalized(LedgerCloseData const& ledgerData,
                                     bool isLatestSlot)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    CLOG_INFO(Ledger,
              "Got consensus: [seq={}, prev={}, txs={}, ops={}, sv: {}]",
              ledgerData.getLedgerSeq(),
              hexAbbrev(ledgerData.getTxSet()->previousLedgerHash()),
              ledgerData.getTxSet()->sizeTxTotal(),
              ledgerData.getTxSet()->sizeOpTotalForLogging(),
              stellarValueToString(mApp.getConfig(), ledgerData.getValue()));

    auto st = getState();
    if (st != LedgerManager::LM_BOOTING_STATE &&
        st != LedgerManager::LM_CATCHING_UP_STATE &&
        st != LedgerManager::LM_SYNCED_STATE)
    {
        releaseAssert(false);
    }

    auto& lam = mApp.getLedgerApplyManager();
    auto res = lam.processLedger(ledgerData, isLatestSlot);
    // Go into catchup if we have any future ledgers we're unable to apply
    // sequentially.
    if (res == LedgerApplyManager::ProcessLedgerResult::
                   WAIT_TO_APPLY_BUFFERED_OR_CATCHUP)
    {
        if (mState != LM_CATCHING_UP_STATE)
        {
            // Out of sync, buffer what we just heard and start catchup.
            CLOG_INFO(Ledger,
                      "Lost sync, local LCL is {}, network closed ledger {}",
                      getLastClosedLedgerHeader().header.ledgerSeq,
                      ledgerData.getLedgerSeq());
        }

        setState(LM_CATCHING_UP_STATE);
    }
}

void
LedgerManagerImpl::startCatchup(
    CatchupConfiguration configuration, std::shared_ptr<HistoryArchive> archive,
    std::set<std::shared_ptr<LiveBucket>> bucketsToRetain)
{
    ZoneScoped;
    setState(LM_CATCHING_UP_STATE);
    mApp.getLedgerApplyManager().startCatchup(configuration, archive,
                                              bucketsToRetain);
}

uint64_t
LedgerManagerImpl::secondsSinceLastLedgerClose() const
{
    uint64_t ct = getLastClosedLedgerHeader().header.scpValue.closeTime;
    if (ct == 0)
    {
        return 0;
    }
    uint64_t now = mApp.timeNow();
    return (now > ct) ? (now - ct) : 0;
}

void
LedgerManagerImpl::syncMetrics()
{
    mLedgerAge.set_count(secondsSinceLastLedgerClose());
    mApp.syncOwnMetrics();
}

void
LedgerManagerImpl::emitNextMeta()
{
    ZoneScoped;

    releaseAssert(mNextMetaToEmit);
    releaseAssert(mMetaStream || mMetaDebugStream);
    auto timer = LogSlowExecution("MetaStream write",
                                  LogSlowExecution::Mode::AUTOMATIC_RAII,
                                  "took", std::chrono::milliseconds(100));
    auto streamWrite = mMetaStreamWriteTime.TimeScope();
    if (mMetaStream)
    {
        size_t written = 0;
        mMetaStream->writeOne(mNextMetaToEmit->getXDR(), nullptr, &written);
        mMetaStream->flush();
        mMetaStreamBytes.Mark(written);
    }
    if (mMetaDebugStream)
    {
        mMetaDebugStream->writeOne(mNextMetaToEmit->getXDR());
        // Flush debug meta in case there's a crash later in commit (in which
        // case we'd lose the data in internal buffers). This way we preserve
        // the meta for problematic ledgers that is vital for diagnostics.
        mMetaDebugStream->flush();
    }
    mNextMetaToEmit.reset();
}

void
maybeSimulateSleep(Config const& cfg, size_t opSize,
                   LogSlowExecution& closeTime)
{
    if (!cfg.OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.empty())
    {
        // Sleep for a parameterized amount of time in simulation mode
        std::discrete_distribution<uint32> distribution(
            cfg.OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.begin(),
            cfg.OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.end());
        std::chrono::microseconds sleepFor{0};
        for (size_t i = 0; i < opSize; i++)
        {
            sleepFor +=
                cfg.OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING[distribution(
                    gRandomEngine)];
        }
        std::chrono::microseconds applicationTime =
            closeTime.checkElapsedTime();
        if (applicationTime < sleepFor)
        {
            sleepFor -= applicationTime;
            CLOG_DEBUG(Perf, "Simulate application: sleep for {} microseconds",
                       sleepFor.count());
            std::this_thread::sleep_for(sleepFor);
        }
    }
}

asio::io_context&
getMetaIOContext(Application& app)
{
    return app.getConfig().parallelLedgerClose()
               ? app.getLedgerCloseIOContext()
               : app.getClock().getIOContext();
}

void
LedgerManagerImpl::ledgerCloseComplete(uint32_t lcl, bool calledViaExternalize,
                                       LedgerCloseData const& ledgerData)
{
    // We just finished applying `lcl`, maybe change LM's state
    // Also notify Herder so it can trigger next ledger.

    releaseAssert(threadIsMain());
    uint32_t latestHeardFromNetwork =
        mApp.getLedgerApplyManager().getLargestLedgerSeqHeard();
    uint32_t latestQueuedToApply =
        mApp.getLedgerApplyManager().getMaxQueuedToApply();
    if (calledViaExternalize)
    {
        releaseAssert(lcl <= latestQueuedToApply);
        releaseAssert(latestQueuedToApply <= latestHeardFromNetwork);
    }

    // Without parallel ledger close, this should always be true
    bool doneApplying = lcl == latestQueuedToApply;
    releaseAssert(doneApplying || mApp.getConfig().parallelLedgerClose());
    if (doneApplying)
    {
        mCurrentlyApplyingLedger = false;
    }

    // Continue execution on the main thread
    // if we have closed the latest ledger we have heard of, set state to
    // "synced"
    bool appliedLatest = false;

    if (latestHeardFromNetwork == lcl)
    {
        mApp.getLedgerManager().moveToSynced();
        appliedLatest = true;
    }

    if (calledViaExternalize)
    {
        // New ledger(s) got closed, notify Herder
        mApp.getHerder().lastClosedLedgerIncreased(appliedLatest,
                                                   ledgerData.getTxSet());
    }
}

/*
    This is the main method that closes the current ledger based on
the close context that was computed by SCP or by the historical module
during replays.

*/
void
LedgerManagerImpl::closeLedger(LedgerCloseData const& ledgerData,
                               bool calledViaExternalize)
{
    if (mApp.isStopping())
    {
        return;
    }

#ifdef BUILD_TESTS
    mLastLedgerTxMeta.clear();
#endif
    ZoneScoped;
    auto ledgerTime = mLedgerClose.TimeScope();
    LogSlowExecution closeLedgerTime{"closeLedger",
                                     LogSlowExecution::Mode::MANUAL, "",
                                     std::chrono::milliseconds::max()};

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    // Note: closeLedger should be able to work correctly based on ledger header
    // stored in LedgerTxn. The issue is that in tests LedgerTxn is sometimes
    // modified manually, which changes ledger header hash compared to the
    // cached one and causes tests to fail.
    LedgerHeader prevHeader = header.current();
#ifdef BUILD_TESTS
    if (threadIsMain())
    {
        prevHeader = getLastClosedLedgerHeader().header;
    }
#endif
    auto prevHash = xdrSha256(prevHeader);

    auto initialLedgerVers = header.current().ledgerVersion;
    ++header.current().ledgerSeq;
    header.current().previousLedgerHash = prevHash;
    CLOG_DEBUG(Ledger, "starting closeLedger() on ledgerSeq={}",
               header.current().ledgerSeq);

    ZoneValue(static_cast<int64_t>(header.current().ledgerSeq));

    auto now = mApp.getClock().now();
    mLedgerAgeClosed.Update(now - mLastClose);
    // mLastClose is only accessed by a single thread, so no synchronization
    // needed
    mLastClose = now;
    mLedgerAge.set_count(0);

    TxSetXDRFrameConstPtr txSet = ledgerData.getTxSet();

    // If we do not support ledger version, we can't apply that ledger, fail!
    if (header.current().ledgerVersion >
        mApp.getConfig().LEDGER_PROTOCOL_VERSION)
    {
        CLOG_ERROR(Ledger, "Unknown ledger version: {}",
                   header.current().ledgerVersion);
        CLOG_ERROR(Ledger, "{}", UPGRADE_STELLAR_CORE);
        throw std::runtime_error(fmt::format(
            FMT_STRING("cannot apply ledger with not supported version: {:d}"),
            header.current().ledgerVersion));
    }

    if (txSet->previousLedgerHash() != prevHash)
    {
        CLOG_ERROR(Ledger, "TxSet mismatch: LCD wants {}, LCL is {}",
                   ledgerAbbrev(ledgerData.getLedgerSeq() - 1,
                                txSet->previousLedgerHash()),
                   ledgerAbbrev(prevHeader));

        CLOG_ERROR(Ledger, "{}", xdrToCerealString(prevHeader, "Full LCL"));
        CLOG_ERROR(Ledger, "{}", POSSIBLY_CORRUPTED_LOCAL_DATA);

        throw std::runtime_error("txset mismatch");
    }

    if (txSet->getContentsHash() != ledgerData.getValue().txSetHash)
    {
        CLOG_ERROR(
            Ledger,
            "Corrupt transaction set: TxSet hash is {}, SCP value reports {}",
            binToHex(txSet->getContentsHash()),
            binToHex(ledgerData.getValue().txSetHash));
        CLOG_ERROR(Ledger, "{}", POSSIBLY_CORRUPTED_QUORUM_SET);

        throw std::runtime_error("corrupt transaction set");
    }

    auto const& sv = ledgerData.getValue();
    header.current().scpValue = sv;

    maybeResetLedgerCloseMetaDebugStream(header.current().ledgerSeq);
    auto applicableTxSet = txSet->prepareForApply(mApp);

    if (applicableTxSet == nullptr)
    {
        CLOG_ERROR(
            Ledger,
            "Corrupt transaction set: TxSet cannot be prepared for apply",
            binToHex(txSet->getContentsHash()),
            binToHex(ledgerData.getValue().txSetHash));
        CLOG_ERROR(Ledger, "{}", POSSIBLY_CORRUPTED_QUORUM_SET);
        throw std::runtime_error("transaction set cannot be processed");
    }

    // In addition to the _canonical_ LedgerResultSet hashed into the
    // LedgerHeader, we optionally collect an even-more-fine-grained record of
    // the ledger entries modified by each tx during tx processing in a
    // LedgerCloseMeta, for streaming to attached clients (typically: horizon).
    std::unique_ptr<LedgerCloseMetaFrame> ledgerCloseMeta;
    if (mMetaStream || mMetaDebugStream)
    {
        if (mNextMetaToEmit)
        {
            releaseAssert(mNextMetaToEmit->ledgerHeader().hash == prevHash);
            emitNextMeta();
        }
        releaseAssert(!mNextMetaToEmit);
        // Write to a local variable rather than a member variable first: this
        // enables us to discard incomplete meta and retry, should anything in
        // this method throw.
        ledgerCloseMeta = std::make_unique<LedgerCloseMetaFrame>(
            header.current().ledgerVersion);
        ledgerCloseMeta->reserveTxProcessing(applicableTxSet->sizeTxTotal());
        ledgerCloseMeta->populateTxSet(*txSet);
    }

    // first, prefetch source accounts for txset, then charge fees
    prefetchTxSourceIds(mApp.getLedgerTxnRoot(), *applicableTxSet,
                        mApp.getConfig());
    auto const mutableTxResults =
        processFeesSeqNums(*applicableTxSet, ltx, ledgerCloseMeta, ledgerData);

    // Subtle: after this call, `header` is invalidated, and is not safe to use
    auto txResultSet = applyTransactions(*applicableTxSet, mutableTxResults,
                                         ltx, ledgerCloseMeta);
    if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
    {
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        mApp.getHistoryManager().appendTransactionSet(ledgerSeq, txSet,
                                                      txResultSet);
    }

    ltx.loadHeader().current().txSetResultHash = xdrSha256(txResultSet);

    // apply any upgrades that were decided during consensus
    // this must be done after applying transactions as the txset
    // was validated before upgrades
    for (size_t i = 0; i < sv.upgrades.size(); i++)
    {
        LedgerUpgrade lupgrade;
        LedgerSnapshot ls(ltx);
        auto valid =
            Upgrades::isValidForApply(sv.upgrades[i], lupgrade, mApp, ls);
        switch (valid)
        {
        case Upgrades::UpgradeValidity::VALID:
            break;
        case Upgrades::UpgradeValidity::XDR_INVALID:
        {
            CLOG_ERROR(Ledger, "Unknown upgrade at index {}", i);
            continue;
        }
        case Upgrades::UpgradeValidity::INVALID:
        {
            CLOG_ERROR(Ledger, "Invalid upgrade at index {}: {}", i,
                       xdrToCerealString(lupgrade, "LedgerUpgrade"));
            continue;
        }
        }

        try
        {
            LedgerTxn ltxUpgrade(ltx);
            Upgrades::applyTo(lupgrade, mApp, ltxUpgrade);

            LedgerEntryChanges changes = ltxUpgrade.getChanges();
            if (ledgerCloseMeta)
            {
                auto& up = ledgerCloseMeta->upgradesProcessing();
                up.emplace_back();
                UpgradeEntryMeta& uem = up.back();
                uem.upgrade = lupgrade;
                uem.changes = changes;
            }
            ltxUpgrade.commit();
        }
        catch (std::runtime_error& e)
        {
            CLOG_ERROR(Ledger, "Exception during upgrade: {}", e.what());
        }
        catch (...)
        {
            CLOG_ERROR(Ledger, "Unknown exception during upgrade");
        }
    }
    auto maybeNewVersion = ltx.loadHeader().current().ledgerVersion;
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    if (protocolVersionStartsFrom(maybeNewVersion, SOROBAN_PROTOCOL_VERSION))
    {
        updateNetworkConfig(ltx);
    }

    auto closeLedgerResult =
        ledgerClosed(ltx, ledgerCloseMeta, initialLedgerVers);

    if (ledgerData.getExpectedHash() &&
        *ledgerData.getExpectedHash() != closeLedgerResult.ledgerHeader.hash)
    {
        throw std::runtime_error("Local node's ledger corrupted during close");
    }

    if (mMetaStream || mMetaDebugStream)
    {
        releaseAssert(ledgerCloseMeta);
        ledgerCloseMeta->ledgerHeader() = closeLedgerResult.ledgerHeader;

        // At this point we've got a complete meta and we can store it to the
        // member variable: if we throw while committing below, we will at worst
        // emit duplicate meta, when retrying.
        mNextMetaToEmit = std::move(ledgerCloseMeta);
        emitNextMeta();
    }

    // The next 7 steps happen in a relatively non-obvious, subtle order.
    // This is unfortunate and it would be nice if we could make it not
    // be so subtle, but for the time being this is where we are.
    //
    // 1. Queue any history-checkpoint, _within_ the current
    //    transaction. This way if there's a crash after commit and before
    //    we've published successfully, we'll re-publish on restart.
    //
    // 2. Commit the current transaction.
    //
    // 3. Finalize any new checkpoint files _after_ the commit. If a crash
    // occurs
    //   between commit and this step, core will attempt finalizing files again
    //   on restart.
    //
    // 4. Start background eviction scan for the next ledger, _after_ the commit
    //    so that it takes its snapshot of network setting from the
    //    committed state.
    //
    // 5. Start any queued checkpoint publishing, _after_ the commit so that
    //    it takes its snapshot of history-rows from the committed state, but
    //    _before_ we GC any buckets (because this is the step where the
    //    bucket refcounts are incremented for the duration of the publish).
    //
    // 6. GC unreferenced buckets. Only do this once publishes are in progress.
    //
    // 7. Finally, reflect newly closed ledger in LedgerManager's and Herder's
    // states: maybe move into SYNCED state, trigger next ledger, etc.

    // Step 1. Maybe queue the current checkpoint file for publishing; this
    // should not race with main, since publish on main begins strictly _after_
    // this call.
    auto& hm = mApp.getHistoryManager();
    hm.maybeQueueHistoryCheckpoint(ledgerSeq);

    // step 2
    ltx.commit();

#ifdef BUILD_TESTS
    mLatestTxResultSet = txResultSet;
#endif

    // step 3
    hm.maybeCheckpointComplete(ledgerSeq);

    // Step 4
    if (protocolVersionStartsFrom(initialLedgerVers, SOROBAN_PROTOCOL_VERSION))
    {
        mApp.getBucketManager().startBackgroundEvictionScan(
            ledgerSeq + 1, initialLedgerVers,
            getSorobanNetworkConfigForApply());
    }

    // Invoke completion handler on the _main_ thread: kick off publishing,
    // cleanup bucket files, notify herder to trigger next ledger
    auto completionHandler = [this, ledgerSeq, calledViaExternalize, ledgerData,
                              ledgerOutput =
                                  std::move(closeLedgerResult)]() mutable {
        releaseAssert(threadIsMain());
        advanceLedgerPointers(ledgerOutput);

        // Step 5. Maybe kick off publishing on complete checkpoint files
        auto& hm = mApp.getHistoryManager();
        hm.publishQueuedHistory();
        hm.logAndUpdatePublishStatus();

        // Step 6. Clean up unreferenced buckets post-apply
        {
            // Ledger state might be updated at the same time, so protect GC
            // call with state mutex
            std::lock_guard<std::recursive_mutex> guard(mLedgerStateMutex);
            mApp.getBucketManager().forgetUnreferencedBuckets(
                getLastClosedLedgerHAS());
        }

        // Step 7. Maybe set LedgerManager into synced state, maybe let
        // Herder trigger next ledger
        ledgerCloseComplete(ledgerSeq, calledViaExternalize, ledgerData);
        CLOG_INFO(Ledger, "Ledger close complete: {}", ledgerSeq);
    };

    if (threadIsMain())
    {
        completionHandler();
    }
    else
    {
        mApp.postOnMainThread(completionHandler, "ledgerCloseComplete");
    }

    maybeSimulateSleep(mApp.getConfig(), txSet->sizeOpTotalForLogging(),
                       closeLedgerTime);
    std::chrono::duration<double> ledgerTimeSeconds = ledgerTime.Stop();
    CLOG_DEBUG(Perf, "Applied ledger {} in {} seconds", ledgerSeq,
               ledgerTimeSeconds.count());
    FrameMark;
}
void
LedgerManagerImpl::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    ZoneScoped;
    if (mApp.getConfig().parallelLedgerClose())
    {
        auto session =
            std::make_unique<soci::session>(mApp.getDatabase().getPool());
        LedgerHeaderUtils::deleteOldEntries(*session, ledgerSeq, count);
    }
    else
    {
        LedgerHeaderUtils::deleteOldEntries(db.getRawSession(), ledgerSeq,
                                            count);
    }
}

void
LedgerManagerImpl::setLastClosedLedger(
    LedgerHeaderHistoryEntry const& lastClosed, bool storeInDB)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    header.current() = lastClosed.header;
    auto has = storeCurrentLedger(header.current(), storeInDB,
                                  /* appendToCheckpoint */ false);
    ltx.commit();

    mRebuildInMemoryState = false;
    advanceLedgerPointers(advanceLedgerStateSnapshot(lastClosed.header, has));

    LedgerTxn ltx2(mApp.getLedgerTxnRoot());
    if (protocolVersionStartsFrom(ltx2.loadHeader().current().ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        mApp.getLedgerManager().updateNetworkConfig(ltx2);
    }
}

void
LedgerManagerImpl::manuallyAdvanceLedgerHeader(LedgerHeader const& header)
{
    if (!mApp.getConfig().MANUAL_CLOSE || !mApp.getConfig().RUN_STANDALONE)
    {
        throw std::logic_error(
            "May only manually advance ledger header sequence number with "
            "MANUAL_CLOSE and RUN_STANDALONE");
    }
    HistoryArchiveState has;
    has.fromString(mApp.getPersistentState().getState(
        PersistentState::kHistoryArchiveState,
        mApp.getDatabase().getSession()));
    auto output = advanceLedgerStateSnapshot(header, has);
    advanceLedgerPointers(output);
}

void
LedgerManagerImpl::setupLedgerCloseMetaStream()
{
    ZoneScoped;

    if (mMetaStream)
    {
        throw std::runtime_error("LedgerManagerImpl already streaming");
    }
    auto& cfg = mApp.getConfig();
    if (cfg.METADATA_OUTPUT_STREAM != "")
    {
        // We can't be sure we're writing to a stream that supports fsync;
        // pipes typically error when you try. So we don't do it.
        mMetaStream =
            std::make_unique<XDROutputFileStream>(getMetaIOContext(mApp),
                                                  /*fsyncOnClose=*/false);
        std::regex fdrx("^fd:([0-9]+)$");
        std::smatch sm;
        if (std::regex_match(cfg.METADATA_OUTPUT_STREAM, sm, fdrx))
        {
            int fd = std::stoi(sm[1]);
            CLOG_INFO(Ledger, "Streaming metadata to file descriptor {}", fd);
            mMetaStream->fdopen(fd);
        }
        else
        {
            CLOG_INFO(Ledger, "Streaming metadata to '{}'",
                      cfg.METADATA_OUTPUT_STREAM);
            mMetaStream->open(cfg.METADATA_OUTPUT_STREAM);
        }
    }
}
void
LedgerManagerImpl::maybeResetLedgerCloseMetaDebugStream(uint32_t ledgerSeq)
{
    ZoneScoped;

    if (mApp.getConfig().METADATA_DEBUG_LEDGERS != 0)
    {
        if (mMetaDebugStream)
        {
            if (!metautils::isDebugSegmentBoundary(ledgerSeq))
            {
                // If we've got a stream open and aren't at a reset boundary,
                // just return -- keep streaming into it.
                return;
            }

            // If we have an open stream and there is already a flush-and-rotate
            // work _running_ (measured by the existence of a shared_ptr at the
            // end of the weak_ptr), we're in a slightly awkward position since
            // we can't synchronously close the stream (that would fsync) and we
            // can't asynchronously close the stream either (that would race
            // against the running work that is presumably flushing-and-rotating
            // the _previous_ stream). So we just skip an iteration of launching
            // flush-and-rotate work, and try again on the next boundary.
            auto existing = mFlushAndRotateMetaDebugWork.lock();
            if (existing && !existing->isDone())
            {
                CLOG_DEBUG(Ledger,
                           "Skipping flush-and-rotate of {} since work already "
                           "running",
                           mMetaDebugPath.string());
                return;
            }

            // If we are resetting and already have a stream, hand it off to the
            // flush-and-rotate work to finish up with.
            mFlushAndRotateMetaDebugWork =
                mApp.getWorkScheduler()
                    .scheduleWork<FlushAndRotateMetaDebugWork>(
                        mMetaDebugPath, std::move(mMetaDebugStream),
                        mApp.getConfig().METADATA_DEBUG_LEDGERS);
            mMetaDebugPath.clear();
        }

        // From here on we're starting a new stream, whether it's the first
        // such stream or a replacement for the one we just handed off to
        // flush-and-rotate. Either way, we should not have an existing one!
        releaseAssert(!mMetaDebugStream);
        auto tmpStream =
            std::make_unique<XDROutputFileStream>(getMetaIOContext(mApp),
                                                  /*fsyncOnClose=*/true);

        auto metaDebugPath = metautils::getMetaDebugFilePath(
            mApp.getBucketManager().getBucketDir(), ledgerSeq);
        releaseAssert(metaDebugPath.has_parent_path());
        try
        {
            if (fs::mkpath(metaDebugPath.parent_path().string()))
            {
                // Skip any files for the same ledger. This is useful in case of
                // a crash-and-restart, where core emits duplicate meta (which
                // isn't garbage-collected until core gets unstuck, risking a
                // disk bloat).
                auto const regexForLedger =
                    metautils::getDebugMetaRegexForLedger(ledgerSeq);
                auto files = fs::findfiles(metaDebugPath.parent_path().string(),
                                           [&](std::string const& file) {
                                               return std::regex_match(
                                                   file, regexForLedger);
                                           });
                if (files.empty())
                {
                    CLOG_DEBUG(Ledger, "Streaming debug metadata to '{}'",
                               metaDebugPath.string());
                    tmpStream->open(metaDebugPath.string());

                    // If we get to this line, the stream is open.
                    mMetaDebugStream = std::move(tmpStream);
                    mMetaDebugPath = metaDebugPath;
                }
            }
            else
            {
                CLOG_WARNING(Ledger,
                             "Failed to make directory '{}' for debug metadata",
                             metaDebugPath.parent_path().string());
            }
        }
        catch (std::runtime_error& e)
        {
            CLOG_WARNING(Ledger,
                         "Failed to open debug metadata stream '{}': {}",
                         metaDebugPath.string(), e.what());
        }
    }
}

SearchableSnapshotConstPtr
LedgerManagerImpl::getCurrentLedgerStateSnaphot()
{
    if (!mReadOnlyLedgerStateSnapshot)
    {
        mReadOnlyLedgerStateSnapshot =
            mApp.getBucketManager()
                .getBucketSnapshotManager()
                .copySearchableLiveBucketListSnapshot();
    }
    return mReadOnlyLedgerStateSnapshot;
}

void
LedgerManagerImpl::advanceLedgerPointers(CloseLedgerOutput const& output)
{
    releaseAssert(threadIsMain());
    CLOG_DEBUG(
        Ledger, "Advancing LCL: {} -> {}", ledgerAbbrev(mLastClosedLedger),
        ledgerAbbrev(output.ledgerHeader.header, output.ledgerHeader.hash));

    // Update ledger state as seen by the main thread
    mLastClosedLedger = output.ledgerHeader;
    mLastClosedLedgerHAS = output.has;
    mSorobanNetworkConfigReadOnly = output.sorobanConfig;
    mReadOnlyLedgerStateSnapshot = output.snapshot;
}

LedgerManagerImpl::CloseLedgerOutput
LedgerManagerImpl::advanceLedgerStateSnapshot(LedgerHeader const& header,
                                              HistoryArchiveState const& has)
{
    auto ledgerHash = xdrSha256(header);

    CloseLedgerOutput res;
    res.ledgerHeader.hash = ledgerHash;
    res.ledgerHeader.header = header;
    res.has = has;
    res.sorobanConfig = mSorobanNetworkConfigForApply;

    auto& bm = mApp.getBucketManager();
    auto liveSnapshot = std::make_unique<BucketListSnapshot<LiveBucket>>(
        bm.getLiveBucketList(), header);
    auto hotArchiveSnapshot =
        std::make_unique<BucketListSnapshot<HotArchiveBucket>>(
            bm.getHotArchiveBucketList(), header);
    // Updating BL snapshot is thread-safe
    bm.getBucketSnapshotManager().updateCurrentSnapshot(
        std::move(liveSnapshot), std::move(hotArchiveSnapshot));

    res.snapshot =
        bm.getBucketSnapshotManager().copySearchableLiveBucketListSnapshot();
    return res;
}

void
LedgerManagerImpl::updateNetworkConfig(AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        if (!mSorobanNetworkConfigForApply)
        {
            mSorobanNetworkConfigForApply =
                std::make_shared<SorobanNetworkConfig>();
        }
        mSorobanNetworkConfigForApply->loadFromLedger(
            ltx, mApp.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION,
            ledgerVersion);
        publishSorobanMetrics();
    }
    else
    {
        throw std::runtime_error("Protocol version is before 20: "
                                 "cannot load Soroban network config");
    }
}

static bool
mergeOpInTx(std::vector<Operation> const& ops)
{
    for (auto const& op : ops)
    {
        if (op.body.type() == ACCOUNT_MERGE)
        {
            return true;
        }
    }
    return false;
}

std::vector<MutableTxResultPtr>
LedgerManagerImpl::processFeesSeqNums(
    ApplicableTxSetFrame const& txSet, AbstractLedgerTxn& ltxOuter,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    LedgerCloseData const& ledgerData)
{
    ZoneScoped;
    std::vector<MutableTxResultPtr> txResults;
    txResults.reserve(txSet.sizeTxTotal());
    CLOG_DEBUG(Ledger, "processing fees and sequence numbers");
    int index = 0;
    try
    {
        LedgerTxn ltx(ltxOuter);
        auto header = ltx.loadHeader().current();
        std::map<AccountID, SequenceNumber> accToMaxSeq;

#ifdef BUILD_TESTS
        // If we have expected results, we assign them to the mutable tx results
        // here.
        std::optional<std::vector<TransactionResultPair>::const_iterator>
            expectedResultsIter = std::nullopt;
        auto expectedResults = ledgerData.getExpectedResults();
        if (expectedResults)
        {
            expectedResultsIter =
                std::make_optional(expectedResults->results.begin());
        }
#endif

        bool mergeSeen = false;
        for (auto const& phase : txSet.getPhasesInApplyOrder())
        {
            for (auto const& tx : phase)
            {
                LedgerTxn ltxTx(ltx);
                txResults.push_back(
                    tx->processFeeSeqNum(ltxTx, txSet.getTxBaseFee(tx)));
#ifdef BUILD_TESTS
                if (expectedResultsIter)
                {
                    releaseAssert(*expectedResultsIter !=
                                  expectedResults->results.end());
                    releaseAssert((*expectedResultsIter)->transactionHash ==
                                  tx->getContentsHash());
                    txResults.back()->setReplayTransactionResult(
                        (*expectedResultsIter)->result);
                    ++(*expectedResultsIter);
                }
#endif // BUILD_TESTS

                if (protocolVersionStartsFrom(
                        ltxTx.loadHeader().current().ledgerVersion,
                        ProtocolVersion::V_19))
                {
                    auto res =
                        accToMaxSeq.emplace(tx->getSourceID(), tx->getSeqNum());
                    if (!res.second)
                    {
                        res.first->second =
                            std::max(res.first->second, tx->getSeqNum());
                    }

                    if (mergeOpInTx(tx->getRawOperations()))
                    {
                        mergeSeen = true;
                    }
                }

                LedgerEntryChanges changes = ltxTx.getChanges();
                if (ledgerCloseMeta)
                {
                    ledgerCloseMeta->pushTxProcessingEntry();
                    ledgerCloseMeta->setLastTxProcessingFeeProcessingChanges(
                        changes);
                }
                ++index;
                ltxTx.commit();
            }
        }
        if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                      ProtocolVersion::V_19) &&
            mergeSeen)
        {
            for (const auto& [accountID, seqNum] : accToMaxSeq)
            {
                auto ltxe = loadMaxSeqNumToApply(ltx, accountID);
                if (!ltxe)
                {
                    InternalLedgerEntry gle(
                        InternalLedgerEntryType::MAX_SEQ_NUM_TO_APPLY);
                    gle.maxSeqNumToApplyEntry().sourceAccount = accountID;
                    gle.maxSeqNumToApplyEntry().maxSeqNum = seqNum;

                    auto res = ltx.create(gle);
                    if (!res)
                    {
                        throw std::runtime_error("create failed");
                    }
                }
                else
                {
                    throw std::runtime_error(
                        "found unexpected MAX_SEQ_NUM_TO_APPLY");
                }
            }
        }

        ltx.commit();
    }
    catch (std::exception& e)
    {
        CLOG_FATAL(Ledger, "processFeesSeqNums error @ {} : {}", index,
                   e.what());
        CLOG_FATAL(Ledger, "{}", REPORT_INTERNAL_BUG);
        throw;
    }

    return txResults;
}

void
LedgerManagerImpl::prefetchTxSourceIds(AbstractLedgerTxnParent& ltx,
                                       ApplicableTxSetFrame const& txSet,
                                       Config const& config)
{
    ZoneScoped;
    if (config.PREFETCH_BATCH_SIZE > 0)
    {
        UnorderedSet<LedgerKey> keys;
        for (auto const& phase : txSet.getPhases())
        {
            for (auto const& tx : phase)
            {
                tx->insertKeysForFeeProcessing(keys);
            }
        }
        ltx.prefetchClassic(keys);
    }
}

void
LedgerManagerImpl::prefetchTransactionData(AbstractLedgerTxnParent& ltx,
                                           ApplicableTxSetFrame const& txSet,
                                           Config const& config)
{
    ZoneScoped;
    if (config.PREFETCH_BATCH_SIZE > 0)
    {
        UnorderedSet<LedgerKey> sorobanKeys;
        auto lkMeter = make_unique<LedgerKeyMeter>();
        UnorderedSet<LedgerKey> classicKeys;
        for (auto const& phase : txSet.getPhases())
        {
            for (auto const& tx : phase)
            {
                if (tx->isSoroban())
                {
                    tx->insertKeysForTxApply(sorobanKeys, lkMeter.get());
                }
                else
                {
                    tx->insertKeysForTxApply(classicKeys, nullptr);
                }
            }
        }
        // Prefetch classic and soroban keys separately for greater
        // visibility into the performance of each mode.
        if (!sorobanKeys.empty())
        {
            ltx.prefetchSoroban(sorobanKeys, lkMeter.get());
        }

        ltx.prefetchClassic(classicKeys);
    }
}

TransactionResultSet
LedgerManagerImpl::applyTransactions(
    ApplicableTxSetFrame const& txSet,
    std::vector<MutableTxResultPtr> const& mutableTxResults,
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta)
{
    ZoneNamedN(txsZone, "applyTransactions", true);
    size_t numTxs = txSet.sizeTxTotal();
    size_t numOps = txSet.sizeOpTotal();
    releaseAssert(numTxs == mutableTxResults.size());
    int index = 0;

    // Record counts
    if (numTxs > 0)
    {
        mTransactionCount.Update(static_cast<int64_t>(numTxs));
        TracyPlot("ledger.transaction.count", static_cast<int64_t>(numTxs));

        mOperationCount.Update(static_cast<int64_t>(numOps));
        TracyPlot("ledger.operation.count", static_cast<int64_t>(numOps));
        CLOG_INFO(Tx, "applying ledger {} ({})",
                  ltx.loadHeader().current().ledgerSeq, txSet.summary());
    }
    TransactionResultSet txResultSet;
    txResultSet.results.reserve(numTxs);

    prefetchTransactionData(mApp.getLedgerTxnRoot(), txSet, mApp.getConfig());
    auto phases = txSet.getPhasesInApplyOrder();

    Hash sorobanBasePrngSeed = txSet.getContentsHash();
    uint64_t txNum{0};
    uint64_t txSucceeded{0};
    uint64_t txFailed{0};
    uint64_t sorobanTxSucceeded{0};
    uint64_t sorobanTxFailed{0};
    size_t resultIndex = 0;
    for (auto const& phase : phases)
    {
        for (auto const& tx : phase)
        {
            ZoneNamedN(txZone, "applyTransaction", true);
            auto mutableTxResult = mutableTxResults.at(resultIndex++);

            auto txTime = mTransactionApply.TimeScope();
            TransactionMetaFrame tm(ltx.loadHeader().current().ledgerVersion);
            CLOG_DEBUG(Tx, " tx#{} = {} ops={} txseq={} (@ {})", index,
                       hexAbbrev(tx->getContentsHash()), tx->getNumOperations(),
                       tx->getSeqNum(),
                       mApp.getConfig().toShortString(tx->getSourceID()));

            Hash subSeed = sorobanBasePrngSeed;
            // If tx can use the seed, we need to compute a sub-seed for it.
            if (tx->isSoroban())
            {
                SHA256 subSeedSha;
                subSeedSha.add(sorobanBasePrngSeed);
                subSeedSha.add(xdr::xdr_to_opaque(txNum));
                subSeed = subSeedSha.finish();
            }
            ++txNum;

            TransactionResultPair results;
            results.transactionHash = tx->getContentsHash();

            tx->apply(mApp.getAppConnector(), ltx, tm, mutableTxResult,
                      subSeed);
            tx->processPostApply(mApp.getAppConnector(), ltx, tm,
                                 mutableTxResult);

            results.result = mutableTxResult->getResult();
            if (results.result.result.code() ==
                TransactionResultCode::txSUCCESS)
            {
                if (tx->isSoroban())
                {
                    ++sorobanTxSucceeded;
                }
                ++txSucceeded;
            }
            else
            {
                if (tx->isSoroban())
                {
                    ++sorobanTxFailed;
                }
                ++txFailed;
            }

            // First gather the TransactionResultPair into the TxResultSet for
            // hashing into the ledger header.
            txResultSet.results.emplace_back(results);
#ifdef BUILD_TESTS
            mLastLedgerTxMeta.push_back(tm);
#endif

            // Then potentially add that TRP and its associated
            // TransactionMeta into the associated slot of any
            // LedgerCloseMeta we're collecting.
            if (ledgerCloseMeta)
            {
                ledgerCloseMeta->setTxProcessingMetaAndResultPair(
                    tm.getXDR(), std::move(results), index);
            }

            ++index;
        }
    }

    mTransactionApplySucceeded.inc(txSucceeded);
    mTransactionApplyFailed.inc(txFailed);
    mSorobanTransactionApplySucceeded.inc(sorobanTxSucceeded);
    mSorobanTransactionApplyFailed.inc(sorobanTxFailed);
    logTxApplyMetrics(ltx, numTxs, numOps);
    return txResultSet;
}

void
LedgerManagerImpl::logTxApplyMetrics(AbstractLedgerTxn& ltx, size_t numTxs,
                                     size_t numOps)
{
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto hitRate = mApp.getLedgerTxnRoot().getPrefetchHitRate() * 100;

    CLOG_DEBUG(Ledger, "Ledger: {} txs: {}, ops: {}, prefetch hit rate (%): {}",
               ledgerSeq, numTxs, numOps, hitRate);

    // We lose a bit of precision here, as medida only accepts int64_t
    mPrefetchHitRate.Update(std::llround(hitRate));
    TracyPlot("ledger.prefetch.hit-rate", hitRate);
}

HistoryArchiveState
LedgerManagerImpl::storeCurrentLedger(LedgerHeader const& header,
                                      bool storeHeader, bool appendToCheckpoint)
{
    ZoneScoped;

    Hash hash = xdrSha256(header);
    releaseAssert(!isZero(hash));
    auto& sess = mApp.getLedgerTxnRoot().getSession();
    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(hash), sess);

    if (mApp.getConfig().ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING.count() >
        0)
    {
        std::this_thread::sleep_for(
            mApp.getConfig().ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING);
    }

    LiveBucketList bl;
    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        bl = mApp.getBucketManager().getLiveBucketList();
    }
    // Store the current HAS in the database; this is really just to
    // checkpoint the bucketlist so we can survive a restart and re-attach
    // to the buckets.
    HistoryArchiveState has(header.ledgerSeq, bl,
                            mApp.getConfig().NETWORK_PASSPHRASE);

    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString(), sess);

    if (mApp.getConfig().MODE_STORES_HISTORY_LEDGERHEADERS && storeHeader)
    {
        LedgerHeaderUtils::storeInDatabase(mApp.getDatabase(), header, sess);
        if (appendToCheckpoint)
        {
            mApp.getHistoryManager().appendLedgerHeader(header);
        }
    }

    return has;
}

// NB: This is a separate method so a testing subclass can override it.
void
LedgerManagerImpl::transferLedgerEntriesToBucketList(
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    LedgerHeader lh, uint32_t initialLedgerVers)
{
    ZoneScoped;
    // `ledgerClosed` protects this call with a mutex
    std::vector<LedgerEntry> initEntries, liveEntries;
    std::vector<LedgerKey> deadEntries;
    auto blEnabled = mApp.getConfig().MODE_ENABLES_BUCKETLIST;

    // Since snapshots are stored in a LedgerEntry, need to snapshot before
    // sealing the ledger with ltx.getAllEntries
    //
    // Any V20 features must be behind initialLedgerVers check, see comment
    // in LedgerManagerImpl::ledgerClosed
    if (blEnabled &&
        protocolVersionStartsFrom(initialLedgerVers, SOROBAN_PROTOCOL_VERSION))
    {
        {
            auto keys = ltx.getAllTTLKeysWithoutSealing();
            LedgerTxn ltxEvictions(ltx);

            auto evictedState =
                mApp.getBucketManager().resolveBackgroundEvictionScan(
                    ltxEvictions, lh.ledgerSeq, keys, initialLedgerVers,
                    *mSorobanNetworkConfigForApply);

            if (protocolVersionStartsFrom(
                    initialLedgerVers,
                    LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
            {
                std::vector<LedgerKey> restoredKeys;
                auto const& restoredKeyMap = ltx.getRestoredHotArchiveKeys();
                for (auto const& key : restoredKeyMap)
                {
                    // TTL keys are not recorded in the hot archive BucketList
                    if (key.type() == CONTRACT_DATA ||
                        key.type() == CONTRACT_CODE)
                    {
                        restoredKeys.push_back(key);
                    }
                }
                mApp.getBucketManager().addHotArchiveBatch(
                    mApp, lh, evictedState.archivedEntries, restoredKeys, {});
            }

            if (ledgerCloseMeta)
            {
                ledgerCloseMeta->populateEvictedEntries(evictedState);
            }

            ltxEvictions.commit();
        }

        mSorobanNetworkConfigForApply->maybeSnapshotBucketListSize(lh.ledgerSeq,
                                                                   ltx, mApp);
    }

    ltx.getAllEntries(initEntries, liveEntries, deadEntries);
    if (blEnabled)
    {
        mApp.getBucketManager().addLiveBatch(mApp, lh, initEntries, liveEntries,
                                             deadEntries);
    }
}

LedgerManagerImpl::CloseLedgerOutput
LedgerManagerImpl::ledgerClosed(
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    uint32_t initialLedgerVers)
{
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> guard(mLedgerStateMutex);
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto currLedgerVers = ltx.loadHeader().current().ledgerVersion;
    CLOG_TRACE(Ledger,
               "sealing ledger {} with version {}, sending to bucket list",
               ledgerSeq, currLedgerVers);

    // There is a subtle bug in the upgrade path that wasn't noticed until
    // protocol 20. For a ledger that upgrades from protocol vN to vN+1,
    // there are two different assumptions in different parts of the
    // ledger-close path:
    //   - In closeLedger we mostly treat the ledger as being on vN, eg.
    //   during
    //     tx apply and LCM construction.
    //   - In the final stage, when we call ledgerClosed, we pass vN+1
    //   because
    //     the upgrade completed and modified the ltx header, and we fish
    //     the protocol out of the ltx header
    // Before LedgerCloseMetaV1, this inconsistency was mostly harmless
    // since LedgerCloseMeta was not modified after the LTX header was
    // modified. However, starting with protocol 20, LedgerCloseMeta is
    // modified after updating the ltx header when populating BucketList
    // related meta. This means that this function will attempt to call
    // LedgerCloseMetaV1 functions, but ledgerCloseMeta is actually a
    // LedgerCloseMetaV0 because it was constructed with the previous
    // protocol version prior to the upgrade. Due to this, we must check the
    // initial protocol version of ledger instead of the ledger version of
    // the current ltx header, which may have been modified via an upgrade.
    transferLedgerEntriesToBucketList(
        ltx, ledgerCloseMeta, ltx.loadHeader().current(), initialLedgerVers);
    if (ledgerCloseMeta &&
        protocolVersionStartsFrom(initialLedgerVers, SOROBAN_PROTOCOL_VERSION))
    {
        ledgerCloseMeta->setNetworkConfiguration(
            getSorobanNetworkConfigForApply(),
            mApp.getConfig().EMIT_LEDGER_CLOSE_META_EXT_V1);
    }

    CloseLedgerOutput res;
    ltx.unsealHeader([this, &res](LedgerHeader& lh) {
        mApp.getBucketManager().snapshotLedger(lh);
        auto has = storeCurrentLedger(lh, /* storeHeader */ true,
                                      /* appendToCheckpoint */ true);
        res = advanceLedgerStateSnapshot(lh, has);
    });

    return res;
}
}
