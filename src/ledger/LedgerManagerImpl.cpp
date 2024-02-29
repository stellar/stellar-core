// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManagerImpl.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/AssumeStateWork.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/HerderPersistence.h"
#include "herder/HerderUtils.h"
#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "herder/Upgrades.h"
#include "history/HistoryManager.h"
#include "ledger/FlushAndRotateMetaDebugWork.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "overlay/OverlayManager.h"
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
#include "util/XDROperators.h"
#include "util/XDRStream.h"
#include "work/WorkScheduler.h"

#include <fmt/format.h>

#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"

#include "medida/buckets.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include <Tracy.hpp>

#include <chrono>
#include <numeric>
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
LedgerManagerImpl::setState(State s)
{
    if (s != getState())
    {
        std::string oldState = getStateHuman();
        mState = s;
        mApp.syncOwnMetrics();
        CLOG_INFO(Ledger, "Changing state {} -> {}", oldState, getStateHuman());
        if (mState != LM_CATCHING_UP_STATE)
        {
            mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
        }

        if (mState == LM_CATCHING_UP_STATE && !mStartCatchup)
        {
            mStartCatchup = std::make_unique<VirtualClock::time_point>(
                mApp.getClock().now());
        }
        else if (mState == LM_SYNCED_STATE && mStartCatchup)
        {
            std::chrono::nanoseconds duration =
                mApp.getClock().now() - *mStartCatchup;
            mCatchupDuration.Update(duration);
            CLOG_DEBUG(Perf, "Caught up to the network in {} seconds",
                       std::chrono::duration<double>(duration).count());
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
    ledgerClosed(ltx, /*ledgerCloseMeta*/ nullptr, /*initialLedgerVers*/ 0);
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
LedgerManagerImpl::loadLastKnownLedger(bool restoreBucketlist,
                                       bool isLedgerStateReady)
{
    ZoneScoped;

    // Step 1. Load LCL state from the DB and extract latest ledger hash
    string lastLedger =
        mApp.getPersistentState().getState(PersistentState::kLastClosedLedger);

    if (lastLedger.empty())
    {
        throw std::runtime_error(
            "No reference in DB to any last closed ledger");
    }

    CLOG_INFO(Ledger, "Last closed ledger (LCL) hash is {}", lastLedger);
    Hash lastLedgerHash = hexToBin256(lastLedger);

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
            HistoryArchiveState has = getLastClosedLedgerHAS();
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

    // Step 3. Restore BucketList if we're doing a full core startup
    // (startServices=true), OR when using BucketListDB
    if (restoreBucketlist || mApp.getConfig().isUsingBucketListDB())
    {
        HistoryArchiveState has = getLastClosedLedgerHAS();
        auto missing = mApp.getBucketManager().checkForMissingBucketsFiles(has);
        auto pubmissing = mApp.getHistoryManager()
                              .getMissingBucketsReferencedByPublishQueue();
        missing.insert(missing.end(), pubmissing.begin(), pubmissing.end());
        if (!missing.empty())
        {
            CLOG_ERROR(Ledger,
                       "{} buckets are missing from bucket directory '{}'",
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
    }

    // Step 4. Restore LedgerManager's internal state
    advanceLedgerPointers(*latestLedgerHeader);

    if (protocolVersionStartsFrom(latestLedgerHeader->ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        if (isLedgerStateReady)
        {
            // Step 5. If ledger state is ready and core is in v20, load network
            // configs right away
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            updateNetworkConfig(ltx);
        }
        else
        {
            // In some modes, e.g. in-memory, core's state is rebuilt
            // asynchronously via catchup. In this case, we're not able to load
            // the network config at this time, and instead must let catchup do
            // it when ready.
            CLOG_INFO(Ledger,
                      "Ledger state is being rebuilt, network config will "
                      "be loaded once the rebuild is done");
        }
    }
}

bool
LedgerManagerImpl::rebuildingInMemoryState()
{
    return mRebuildInMemoryState;
}

void
LedgerManagerImpl::setupInMemoryStateRebuild()
{
    if (!mRebuildInMemoryState)
    {
        LedgerHeader lh;
        HistoryArchiveState has;
        auto& ps = mApp.getPersistentState();
        ps.setState(PersistentState::kLastClosedLedger,
                    binToHex(xdrSha256(lh)));
        ps.setState(PersistentState::kHistoryArchiveState, has.toString());
        ps.setState(PersistentState::kLastSCPData, "");
        ps.setState(PersistentState::kLastSCPDataXDR, "");
        ps.setState(PersistentState::kLedgerUpgrades, "");
        mRebuildInMemoryState = true;
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
    return mLastClosedLedger.header.maxTxSetSize;
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSizeOps() const
{
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
        auto conf = getSorobanNetworkConfig();
        std::vector<int64_t> limits = {conf.ledgerMaxTxCount(),
                                       conf.ledgerMaxInstructions(),
                                       conf.ledgerMaxTransactionSizesBytes(),
                                       conf.ledgerMaxReadBytes(),
                                       conf.ledgerMaxWriteBytes(),
                                       conf.ledgerMaxReadLedgerEntries(),
                                       conf.ledgerMaxWriteLedgerEntries()};
        return Resource(limits);
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

    auto const& conf = mApp.getLedgerManager().getSorobanNetworkConfig();
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
    auto const& lh = mLastClosedLedger.header;
    if (protocolVersionIsBefore(lh.ledgerVersion, ProtocolVersion::V_9))
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2LL + ownerCount) * int64_t(lh.baseReserve);
}

uint32_t
LedgerManagerImpl::getLastReserve() const
{
    return mLastClosedLedger.header.baseReserve;
}

uint32_t
LedgerManagerImpl::getLastTxFee() const
{
    return mLastClosedLedger.header.baseFee;
}

LedgerHeaderHistoryEntry const&
LedgerManagerImpl::getLastClosedLedgerHeader() const
{
    return mLastClosedLedger;
}

HistoryArchiveState
LedgerManagerImpl::getLastClosedLedgerHAS()
{
    ZoneScoped;

    string hasString = mApp.getPersistentState().getState(
        PersistentState::kHistoryArchiveState);
    HistoryArchiveState has;
    has.fromString(hasString);
    return has;
}

uint32_t
LedgerManagerImpl::getLastClosedLedgerNum() const
{
    return mLastClosedLedger.header.ledgerSeq;
}

SorobanNetworkConfig&
LedgerManagerImpl::getSorobanNetworkConfigInternal()
{
    releaseAssert(mSorobanNetworkConfig);
    return *mSorobanNetworkConfig;
}

SorobanNetworkConfig const&
LedgerManagerImpl::getSorobanNetworkConfig()
{
    return getSorobanNetworkConfigInternal();
}

bool
LedgerManagerImpl::hasSorobanNetworkConfig() const
{
    return mSorobanNetworkConfig.has_value();
}

#ifdef BUILD_TESTS
SorobanNetworkConfig&
LedgerManagerImpl::getMutableSorobanNetworkConfig()
{
    return getSorobanNetworkConfigInternal();
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
    releaseAssert(mSorobanNetworkConfig);
    // first publish the network config limits
    auto contractMaxSizeBytes = mSorobanNetworkConfig->maxContractSizeBytes();
    auto ledgerMaxInstructions = mSorobanNetworkConfig->ledgerMaxInstructions();
    auto txMaxInstructions = mSorobanNetworkConfig->txMaxInstructions();
    auto txMemoryLimit = mSorobanNetworkConfig->txMemoryLimit();
    auto ledgerMaxReadLedgerEntries =
        mSorobanNetworkConfig->ledgerMaxReadLedgerEntries();
    auto ledgerMaxReadBytes = mSorobanNetworkConfig->ledgerMaxReadBytes();
    auto ledgerMaxWriteLedgerEntries =
        mSorobanNetworkConfig->ledgerMaxWriteLedgerEntries();
    auto ledgerMaxWriteBytes = mSorobanNetworkConfig->ledgerMaxWriteBytes();
    auto txMaxReadLedgerEntries =
        mSorobanNetworkConfig->txMaxReadLedgerEntries();
    auto txMaxReadBytes = mSorobanNetworkConfig->txMaxReadBytes();
    auto txMaxWriteLedgerEntries =
        mSorobanNetworkConfig->txMaxWriteLedgerEntries();
    auto txMaxWriteBytes = mSorobanNetworkConfig->txMaxWriteBytes();
    auto bucketListTargetSizeBytes =
        mSorobanNetworkConfig->bucketListTargetSizeBytes();
    auto txMaxContractEventsSizeBytes =
        mSorobanNetworkConfig->txMaxContractEventsSizeBytes();
    auto contractDataKeySizeBytes =
        mSorobanNetworkConfig->maxContractDataKeySizeBytes();
    auto contractDataEntrySizeBytes =
        mSorobanNetworkConfig->maxContractDataEntrySizeBytes();

    mSorobanMetrics.mConfigContractMaxRwKeyByte.set_count(
        contractDataKeySizeBytes);
    mSorobanMetrics.mConfigContractMaxRwDataByte.set_count(
        contractDataEntrySizeBytes);
    mSorobanMetrics.mConfigContractMaxRwCodeByte.set_count(
        contractMaxSizeBytes);
    mSorobanMetrics.mConfigTxMaxCpuInsn.set_count(txMaxInstructions);
    mSorobanMetrics.mConfigTxMaxMemByte.set_count(txMemoryLimit);
    mSorobanMetrics.mConfigTxMaxReadEntry.set_count(txMaxReadLedgerEntries);
    mSorobanMetrics.mConfigTxMaxReadLedgerByte.set_count(txMaxReadBytes);
    mSorobanMetrics.mConfigTxMaxWriteEntry.set_count(txMaxWriteLedgerEntries);
    mSorobanMetrics.mConfigTxMaxWriteLedgerByte.set_count(txMaxWriteBytes);
    mSorobanMetrics.mConfigTxMaxEmitEventByte.set_count(
        txMaxContractEventsSizeBytes);
    mSorobanMetrics.mConfigLedgerMaxCpuInsn.set_count(ledgerMaxInstructions);
    mSorobanMetrics.mConfigLedgerMaxReadEntry.set_count(
        ledgerMaxReadLedgerEntries);
    mSorobanMetrics.mConfigLedgerMaxReadLedgerByte.set_count(
        ledgerMaxReadBytes);
    mSorobanMetrics.mConfigLedgerMaxWriteEntry.set_count(
        ledgerMaxWriteLedgerEntries);
    mSorobanMetrics.mConfigLedgerMaxWriteLedgerByte.set_count(
        ledgerMaxWriteBytes);
    mSorobanMetrics.mConfigBucketListTargetSizeByte.set_count(
        bucketListTargetSizeBytes);

    // then publish the actual ledger usage
    mSorobanMetrics.publishAndResetLedgerWideMetrics();
}

// called by txherder
void
LedgerManagerImpl::valueExternalized(LedgerCloseData const& ledgerData)
{
    ZoneScoped;

    // Capture LCL before we do any processing (which may trigger ledger close)
    auto lcl = getLastClosedLedgerNum();

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

    closeLedgerIf(ledgerData);

    auto& cm = mApp.getCatchupManager();

    cm.processLedger(ledgerData);

    // We set the state to synced
    // if we have closed the latest ledger we have heard of.
    bool appliedLatest = false;
    if (cm.getLargestLedgerSeqHeard() == getLastClosedLedgerNum())
    {
        setState(LM_SYNCED_STATE);
        appliedLatest = true;
    }

    // New ledger(s) got closed, notify Herder
    if (getLastClosedLedgerNum() > lcl)
    {
        CLOG_DEBUG(Ledger,
                   "LedgerManager::valueExternalized LCL advanced {} -> {}",
                   lcl, getLastClosedLedgerNum());
        mApp.getHerder().lastClosedLedgerIncreased(appliedLatest);
    }
    FrameMark;
}

void
LedgerManagerImpl::closeLedgerIf(LedgerCloseData const& ledgerData)
{
    ZoneScoped;
    if (mLastClosedLedger.header.ledgerSeq + 1 == ledgerData.getLedgerSeq())
    {
        auto& cm = mApp.getCatchupManager();
        // if catchup work is running, we don't want ledger manager to close
        // this ledger and potentially cause issues.
        if (cm.isCatchupInitialized() && !cm.catchupWorkIsDone())
        {
            CLOG_INFO(
                Ledger,
                "Can't close ledger: {}  in LM because catchup is running",
                ledgerAbbrev(mLastClosedLedger));
            return;
        }

        closeLedger(ledgerData);
        CLOG_INFO(Ledger, "Closed ledger: {}", ledgerAbbrev(mLastClosedLedger));
    }
    else if (ledgerData.getLedgerSeq() <= mLastClosedLedger.header.ledgerSeq)
    {
        CLOG_INFO(
            Ledger,
            "Skipping close ledger: local state is {}, more recent than {}",
            mLastClosedLedger.header.ledgerSeq, ledgerData.getLedgerSeq());
    }
    else
    {
        if (mState != LM_CATCHING_UP_STATE)
        {
            // Out of sync, buffer what we just heard and start catchup.
            CLOG_INFO(
                Ledger, "Lost sync, local LCL is {}, network closed ledger {}",
                mLastClosedLedger.header.ledgerSeq, ledgerData.getLedgerSeq());
        }

        setState(LM_CATCHING_UP_STATE);
    }
}

void
LedgerManagerImpl::startCatchup(
    CatchupConfiguration configuration, std::shared_ptr<HistoryArchive> archive,
    std::set<std::shared_ptr<Bucket>> bucketsToRetain)
{
    ZoneScoped;
    setState(LM_CATCHING_UP_STATE);
    mApp.getCatchupManager().startCatchup(configuration, archive,
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

/*
    This is the main method that closes the current ledger based on
the close context that was computed by SCP or by the historical module
during replays.

*/
void
LedgerManagerImpl::closeLedger(LedgerCloseData const& ledgerData)
{
    ZoneScoped;
    auto ledgerTime = mLedgerClose.TimeScope();
    LogSlowExecution closeLedgerTime{"closeLedger",
                                     LogSlowExecution::Mode::MANUAL, "",
                                     std::chrono::milliseconds::max()};

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    auto initialLedgerVers = header.current().ledgerVersion;
    ++header.current().ledgerSeq;
    header.current().previousLedgerHash = mLastClosedLedger.hash;
    CLOG_DEBUG(Ledger, "starting closeLedger() on ledgerSeq={}",
               header.current().ledgerSeq);

    ZoneValue(static_cast<int64_t>(header.current().ledgerSeq));

    auto now = mApp.getClock().now();
    mLedgerAgeClosed.Update(now - mLastClose);
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

    if (txSet->previousLedgerHash() != getLastClosedLedgerHeader().hash)
    {
        CLOG_ERROR(Ledger, "TxSet mismatch: LCD wants {}, LCL is {}",
                   ledgerAbbrev(ledgerData.getLedgerSeq() - 1,
                                txSet->previousLedgerHash()),
                   ledgerAbbrev(getLastClosedLedgerHeader()));

        CLOG_ERROR(Ledger, "{}",
                   xdr_to_string(getLastClosedLedgerHeader(), "Full LCL"));
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
            releaseAssert(mNextMetaToEmit->ledgerHeader().hash ==
                          getLastClosedLedgerHeader().hash);
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

    // the transaction set that was agreed upon by consensus
    // was sorted by hash; we reorder it so that transactions are
    // sorted such that sequence numbers are respected
    std::vector<TransactionFrameBasePtr> const txs =
        applicableTxSet->getTxsInApplyOrder();

    // first, prefetch source accounts for txset, then charge fees
    prefetchTxSourceIds(txs);
    processFeesSeqNums(txs, ltx, *applicableTxSet, ledgerCloseMeta);

    TransactionResultSet txResultSet;
    txResultSet.results.reserve(txs.size());
    applyTransactions(*applicableTxSet, txs, ltx, txResultSet, ledgerCloseMeta);
    if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
    {
        storeTxSet(mApp.getDatabase(), ltx.loadHeader().current().ledgerSeq,
                   *txSet);
    }

    ltx.loadHeader().current().txSetResultHash = xdrSha256(txResultSet);

    // apply any upgrades that were decided during consensus
    // this must be done after applying transactions as the txset
    // was validated before upgrades
    for (size_t i = 0; i < sv.upgrades.size(); i++)
    {
        LedgerUpgrade lupgrade;
        auto valid = Upgrades::isValidForApply(sv.upgrades[i], lupgrade, mApp,
                                               ltx, ltx.loadHeader().current());
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
                       xdr_to_string(lupgrade, "LedgerUpgrade"));
            continue;
        }
        }

        try
        {
            LedgerTxn ltxUpgrade(ltx);
            Upgrades::applyTo(lupgrade, mApp, ltxUpgrade);

            auto ledgerSeq = ltxUpgrade.loadHeader().current().ledgerSeq;
            LedgerEntryChanges changes = ltxUpgrade.getChanges();
            if (ledgerCloseMeta)
            {
                auto& up = ledgerCloseMeta->upgradesProcessing();
                up.emplace_back();
                UpgradeEntryMeta& uem = up.back();
                uem.upgrade = lupgrade;
                uem.changes = changes;
            }
            // Note: Index from 1 rather than 0 to match the behavior of
            // storeTransaction and storeTransactionFee.
            if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
            {
                Upgrades::storeUpgradeHistory(getDatabase(), ledgerSeq,
                                              lupgrade, changes,
                                              static_cast<int>(i + 1));
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
    if (protocolVersionStartsFrom(maybeNewVersion, SOROBAN_PROTOCOL_VERSION))
    {
        updateNetworkConfig(ltx);
        mApp.getOverlayManager().dropPeersIf(
            shouldDropPeerPredicate, maybeNewVersion, "version too old");
    }

    ledgerClosed(ltx, ledgerCloseMeta, initialLedgerVers);

    if (ledgerData.getExpectedHash() &&
        *ledgerData.getExpectedHash() != mLastClosedLedger.hash)
    {
        throw std::runtime_error("Local node's ledger corrupted during close");
    }

    if (mMetaStream || mMetaDebugStream)
    {
        releaseAssert(ledgerCloseMeta);
        ledgerCloseMeta->ledgerHeader() = mLastClosedLedger;

        // At this point we've got a complete meta and we can store it to the
        // member variable: if we throw while committing below, we will at worst
        // emit duplicate meta, when retrying.
        mNextMetaToEmit = std::move(ledgerCloseMeta);

        // If the LedgerCloseData provided an expected hash, then we validated
        // it above.
        if (!mApp.getConfig().EXPERIMENTAL_PRECAUTION_DELAY_META ||
            ledgerData.getExpectedHash())
        {
            emitNextMeta();
        }
    }

    // The next 4 steps happen in a relatively non-obvious, subtle order.
    // This is unfortunate and it would be nice if we could make it not
    // be so subtle, but for the time being this is where we are.
    //
    // 1. Queue any history-checkpoint to the database, _within_ the current
    //    transaction. This way if there's a crash after commit and before
    //    we've published successfully, we'll re-publish on restart.
    //
    // 2. Commit the current transaction.
    //
    // 3. Start any queued checkpoint publishing, _after_ the commit so that
    //    it takes its snapshot of history-rows from the committed state, but
    //    _before_ we GC any buckets (because this is the step where the
    //    bucket refcounts are incremented for the duration of the publish).
    //
    // 4. GC unreferenced buckets. Only do this once publishes are in progress.

    // step 1
    auto& hm = mApp.getHistoryManager();
    hm.maybeQueueHistoryCheckpoint();

    // step 2
    ltx.commit();

    // step 3
    hm.publishQueuedHistory();
    hm.logAndUpdatePublishStatus();

    // step 4
    mApp.getBucketManager().forgetUnreferencedBuckets();

    if (!mApp.getConfig().OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.empty())
    {
        // Sleep for a parameterized amount of time in simulation mode
        std::discrete_distribution<uint32> distribution(
            mApp.getConfig().OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.begin(),
            mApp.getConfig().OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.end());
        std::chrono::microseconds sleepFor{0};
        auto txSetSizeOp = applicableTxSet->sizeOpTotal();
        for (size_t i = 0; i < txSetSizeOp; i++)
        {
            sleepFor +=
                mApp.getConfig()
                    .OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING[distribution(
                        gRandomEngine)];
        }
        std::chrono::microseconds applicationTime =
            closeLedgerTime.checkElapsedTime();
        if (applicationTime < sleepFor)
        {
            sleepFor -= applicationTime;
            CLOG_DEBUG(Perf, "Simulate application: sleep for {} microseconds",
                       sleepFor.count());
            std::this_thread::sleep_for(sleepFor);
        }
    }

    std::chrono::duration<double> ledgerTimeSeconds = ledgerTime.Stop();
    CLOG_DEBUG(Perf, "Applied ledger in {} seconds", ledgerTimeSeconds.count());
}

void
LedgerManagerImpl::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    ZoneScoped;
    soci::transaction txscope(db.getSession());
    db.clearPreparedStatementCache();
    LedgerHeaderUtils::deleteOldEntries(db, ledgerSeq, count);
    deleteOldTransactionHistoryEntries(db, ledgerSeq, count);
    HerderPersistence::deleteOldEntries(db, ledgerSeq, count);
    Upgrades::deleteOldEntries(db, ledgerSeq, count);
    db.clearPreparedStatementCache();
    txscope.commit();
}

void
LedgerManagerImpl::deleteNewerEntries(Database& db, uint32_t ledgerSeq)
{
    ZoneScoped;
    soci::transaction txscope(db.getSession());
    db.clearPreparedStatementCache();

    // as we use this method only when we apply buckets, we have to preserve
    // data for everything but ledger header
    LedgerHeaderUtils::deleteNewerEntries(db, ledgerSeq);
    // for other data we delete data *after*
    ++ledgerSeq;
    deleteNewerTransactionHistoryEntries(db, ledgerSeq);
    HerderPersistence::deleteNewerEntries(db, ledgerSeq);
    Upgrades::deleteNewerEntries(db, ledgerSeq);
    db.clearPreparedStatementCache();
    txscope.commit();
}

void
LedgerManagerImpl::setLastClosedLedger(
    LedgerHeaderHistoryEntry const& lastClosed, bool storeInDB)
{
    ZoneScoped;
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    header.current() = lastClosed.header;
    storeCurrentLedger(header.current(), storeInDB);
    ltx.commit();

    mRebuildInMemoryState = false;
    advanceLedgerPointers(lastClosed.header);
    LedgerTxn ltx2(mApp.getLedgerTxnRoot(), false,
                   TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
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
    advanceLedgerPointers(header, false);
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
        mMetaStream = std::make_unique<XDROutputFileStream>(
            mApp.getClock().getIOContext(),
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
        auto tmpStream = std::make_unique<XDROutputFileStream>(
            mApp.getClock().getIOContext(),
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

void
LedgerManagerImpl::advanceLedgerPointers(LedgerHeader const& header,
                                         bool debugLog)
{
    auto ledgerHash = xdrSha256(header);

    if (debugLog)
    {
        CLOG_DEBUG(Ledger, "Advancing LCL: {} -> {}",
                   ledgerAbbrev(mLastClosedLedger),
                   ledgerAbbrev(header, ledgerHash));
    }

    mLastClosedLedger.hash = ledgerHash;
    mLastClosedLedger.header = header;
}

void
LedgerManagerImpl::updateNetworkConfig(AbstractLedgerTxn& rootLtx)
{
    ZoneScoped;

    uint32_t ledgerVersion{};
    {
        LedgerTxn ltx(rootLtx, false,
                      TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
        ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    }

    if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        if (!mSorobanNetworkConfig)
        {
            mSorobanNetworkConfig = std::make_optional<SorobanNetworkConfig>();
        }
        mSorobanNetworkConfig->loadFromLedger(
            rootLtx, mApp.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION,
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

void
LedgerManagerImpl::processFeesSeqNums(
    std::vector<TransactionFrameBasePtr> const& txs,
    AbstractLedgerTxn& ltxOuter, ApplicableTxSetFrame const& txSet,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta)
{
    ZoneScoped;
    CLOG_DEBUG(Ledger, "processing fees and sequence numbers");
    int index = 0;
    try
    {
        LedgerTxn ltx(ltxOuter);
        auto header = ltx.loadHeader().current();
        auto ledgerSeq = header.ledgerSeq;
        std::map<AccountID, SequenceNumber> accToMaxSeq;

        bool mergeSeen = false;
        for (auto tx : txs)
        {
            LedgerTxn ltxTx(ltx);
            tx->processFeeSeqNum(ltxTx, txSet.getTxBaseFee(tx, header));

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
            // Note to future: when we eliminate the txhistory and txfeehistory
            // tables, the following step can be removed.
            //
            // Also note: for historical reasons the history tables number
            // txs counting from 1, not 0. We preserve this for the time being
            // in case anyone depends on it.
            ++index;
            if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
            {
                storeTransactionFee(mApp.getDatabase(), ledgerSeq, tx, changes,
                                    index);
            }
            ltxTx.commit();
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
}

void
LedgerManagerImpl::prefetchTxSourceIds(
    std::vector<TransactionFrameBasePtr> const& txs)
{
    ZoneScoped;
    if (mApp.getConfig().PREFETCH_BATCH_SIZE > 0)
    {
        UnorderedSet<LedgerKey> keys;
        for (auto const& tx : txs)
        {
            tx->insertKeysForFeeProcessing(keys);
        }
        mApp.getLedgerTxnRoot().prefetch(keys);
    }
}

void
LedgerManagerImpl::prefetchTransactionData(
    std::vector<TransactionFrameBasePtr> const& txs)
{
    ZoneScoped;
    if (mApp.getConfig().PREFETCH_BATCH_SIZE > 0)
    {
        UnorderedSet<LedgerKey> keys;
        for (auto const& tx : txs)
        {
            tx->insertKeysForTxApply(keys);
        }
        mApp.getLedgerTxnRoot().prefetch(keys);
    }
}

void
LedgerManagerImpl::applyTransactions(
    ApplicableTxSetFrame const& txSet,
    std::vector<TransactionFrameBasePtr> const& txs, AbstractLedgerTxn& ltx,
    TransactionResultSet& txResultSet,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta)
{
    ZoneNamedN(txsZone, "applyTransactions", true);
    int index = 0;

    // Record counts
    auto numTxs = txs.size();
    auto numOps = txSet.sizeOpTotal();
    if (numTxs > 0)
    {
        mTransactionCount.Update(static_cast<int64_t>(numTxs));
        TracyPlot("ledger.transaction.count", static_cast<int64_t>(numTxs));

        mOperationCount.Update(static_cast<int64_t>(numOps));
        TracyPlot("ledger.operation.count", static_cast<int64_t>(numOps));
        CLOG_INFO(Tx, "applying ledger {} ({})",
                  ltx.loadHeader().current().ledgerSeq, txSet.summary());
    }

    prefetchTransactionData(txs);

    Hash sorobanBasePrngSeed = txSet.getContentsHash();
    uint64_t txNum{0};
    uint64_t txSucceeded{0};
    uint64_t txFailed{0};
    for (auto tx : txs)
    {
        ZoneNamedN(txZone, "applyTransaction", true);
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

        tx->apply(mApp, ltx, tm, subSeed);
        tx->processPostApply(mApp, ltx, tm);
        TransactionResultPair results;
        results.transactionHash = tx->getContentsHash();
        results.result = tx->getResult();
        if (results.result.result.code() == TransactionResultCode::txSUCCESS)
        {
            ++txSucceeded;
        }
        else
        {
            ++txFailed;
        }

        // First gather the TransactionResultPair into the TxResultSet for
        // hashing into the ledger header.
        txResultSet.results.emplace_back(results);

        // Then potentially add that TRP and its associated TransactionMeta
        // into the associated slot of any LedgerCloseMeta we're collecting.
        if (ledgerCloseMeta)
        {
            ledgerCloseMeta->setTxProcessingMetaAndResultPair(
                tm.getXDR(), std::move(results), index);
        }

        // Then finally store the results and meta into the txhistory table.
        // if we're running in a mode that has one.
        //
        // Note to future: when we eliminate the txhistory and txfeehistory
        // tables, the following step can be removed.
        //
        // Also note: for historical reasons the history tables number
        // txs counting from 1, not 0. We preserve this for the time being
        // in case anyone depends on it.
        ++index;
        if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
        {
            auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
            storeTransaction(mApp.getDatabase(), ledgerSeq, tx, tm.getXDR(),
                             txResultSet);
        }
    }

    mTransactionApplySucceeded.inc(txSucceeded);
    mTransactionApplyFailed.inc(txFailed);
    logTxApplyMetrics(ltx, numTxs, numOps);
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

void
LedgerManagerImpl::storeCurrentLedger(LedgerHeader const& header,
                                      bool storeHeader)
{
    ZoneScoped;

    Hash hash = xdrSha256(header);
    releaseAssert(!isZero(hash));
    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(hash));

    BucketList bl;
    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        bl = mApp.getBucketManager().getBucketList();
    }
    // Store the current HAS in the database; this is really just to checkpoint
    // the bucketlist so we can survive a restart and re-attach to the buckets.
    HistoryArchiveState has(header.ledgerSeq, bl,
                            mApp.getConfig().NETWORK_PASSPHRASE);

    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString());

    if (mApp.getConfig().MODE_STORES_HISTORY_LEDGERHEADERS && storeHeader)
    {
        LedgerHeaderUtils::storeInDatabase(mApp.getDatabase(), header);
    }
}

// NB: This is a separate method so a testing subclass can override it.
void
LedgerManagerImpl::transferLedgerEntriesToBucketList(
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    uint32_t ledgerSeq, uint32_t currLedgerVers, uint32_t initialLedgerVers)
{
    ZoneScoped;
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
            LedgerTxn ltxEvictions(ltx);
            mApp.getBucketManager().scanForEviction(ltxEvictions, ledgerSeq);
            if (ledgerCloseMeta)
            {
                ledgerCloseMeta->populateEvictedEntries(
                    ltxEvictions.getChanges());
            }
            ltxEvictions.commit();
        }

        getSorobanNetworkConfigInternal().maybeSnapshotBucketListSize(
            ledgerSeq, ltx, mApp);
    }

    ltx.getAllEntries(initEntries, liveEntries, deadEntries);
    if (blEnabled)
    {
        mApp.getBucketManager().addBatch(mApp, ledgerSeq, currLedgerVers,
                                         initEntries, liveEntries, deadEntries);
    }
}

void
LedgerManagerImpl::ledgerClosed(
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    uint32_t initialLedgerVers)
{
    ZoneScoped;
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto currLedgerVers = ltx.loadHeader().current().ledgerVersion;
    CLOG_TRACE(Ledger,
               "sealing ledger {} with version {}, sending to bucket list",
               ledgerSeq, currLedgerVers);

    // There is a subtle bug in the upgrade path that wasn't noticed until
    // protocol 20. For a ledger that upgrades from protocol vN to vN+1, there
    // are two different assumptions in different parts of the ledger-close
    // path:
    //   - In closeLedger we mostly treat the ledger as being on vN, eg. during
    //     tx apply and LCM construction.
    //   - In the final stage, when we call ledgerClosed, we pass vN+1 because
    //     the upgrade completed and modified the ltx header, and we fish the
    //     protocol out of the ltx header
    // Before LedgerCloseMetaV1, this inconsistency was mostly harmless since
    // LedgerCloseMeta was not modified after the LTX header was modified.
    // However, starting with protocol 20, LedgerCloseMeta is modified after
    // updating the ltx header when populating BucketList related meta. This
    // means that this function will attempt to call LedgerCloseMetaV1
    // functions, but ledgerCloseMeta is actually a LedgerCloseMetaV0 because it
    // was constructed with the previous protocol version prior to the upgrade.
    // Due to this, we must check the initial protocol version of ledger instead
    // of the ledger version of the current ltx header, which may have been
    // modified via an upgrade.
    transferLedgerEntriesToBucketList(ltx, ledgerCloseMeta, ledgerSeq,
                                      currLedgerVers, initialLedgerVers);
    if (ledgerCloseMeta &&
        protocolVersionStartsFrom(initialLedgerVers, SOROBAN_PROTOCOL_VERSION))
    {
        auto blSize = getSorobanNetworkConfig().getAverageBucketListSize();
        ledgerCloseMeta->setTotalByteSizeOfBucketList(blSize);
    }

    ltx.unsealHeader([this](LedgerHeader& lh) {
        mApp.getBucketManager().snapshotLedger(lh);
        storeCurrentLedger(lh, /* storeHeader */ true);
        advanceLedgerPointers(lh);
    });
}
}
