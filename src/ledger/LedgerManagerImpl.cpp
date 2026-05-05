// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Must come before any xdrpp/marshal.h consumer below: declares the
// xdr::detail::bytes_to_void overload that lets xdr::xdr_from_opaque ingest
// `rust::Vec<uint8_t>` byte buffers (used by applySorobanPhaseRust to
// decode bridge outputs).
#include "rust/RustVecXdrMarshal.h"

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
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/FlushAndRotateMetaDebugWork.h"
#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/P23HotArchiveBug.h"
#include "ledger/SharedModuleCacheCompiler.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "rust/RustBridge.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMeta.h"
#include "ledger/LedgerTypeUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/DebugMetaUtils.h"
#include "util/Decoder.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/JitterInjection.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "util/ThreadAnnotations.h"
#include "util/XDRCereal.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include "work/WorkScheduler.h"
#include "xdr/Stellar-ledger-entries.h"

#include <cstdint>
#include <fmt/format.h>

#ifdef BUILD_TESTS
#include "test/TxTests.h"
#endif
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/types.h"

#include "medida/buckets.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include <Tracy.hpp>

#include "LedgerManagerImpl.h"
#include <chrono>
#include <future>
#include <memory>
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

uint32_t const LedgerManager::GENESIS_LEDGER_SEQ = 1;
uint32_t const LedgerManager::GENESIS_LEDGER_VERSION = 0;
uint32_t const LedgerManager::GENESIS_LEDGER_BASE_FEE = 100;
uint32_t const LedgerManager::GENESIS_LEDGER_BASE_RESERVE = 100000000;
uint32_t const LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE = 100;
int64_t const LedgerManager::GENESIS_LEDGER_TOTAL_COINS = 1000000000000000000;

namespace
{

// Read-only ledger-state snapshot used by the Soroban parallel pre-apply
// pass. Wraps an immutable LCL bucket-list snapshot and an immutable
// pre-built overlay of accounts that the in-flight classic phase may
// have touched. Each query checks the overlay first (a hit indicates a
// classic-phase mutation visible to the Soroban phase — including a
// post-classic deletion, represented by a stored null entry) and falls
// back to the bucket snapshot otherwise. All inputs are immutable for
// the lifetime of the parallel pass, so workers can construct their own
// instances and read concurrently without synchronization.
class OverlayApplySnapshot : public AbstractLedgerStateSnapshot
{
    using OverlayMap =
        UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>;
    BucketSnapshotState mInner;
    std::shared_ptr<OverlayMap const> mOverlay;

  public:
    OverlayApplySnapshot(ApplyLedgerStateSnapshot const& snap,
                         std::shared_ptr<OverlayMap const> overlay)
        : mInner(snap), mOverlay(std::move(overlay))
    {
    }

    LedgerHeaderWrapper
    getLedgerHeader() const override
    {
        return mInner.getLedgerHeader();
    }

    LedgerEntryWrapper
    getAccount(AccountID const& account) const override
    {
        auto it = mOverlay->find(accountKey(account));
        if (it != mOverlay->end())
        {
            return LedgerEntryWrapper(it->second);
        }
        return mInner.getAccount(account);
    }

    LedgerEntryWrapper
    getAccount(LedgerHeaderWrapper const& header,
               TransactionFrame const& tx) const override
    {
        return getAccount(tx.getSourceID());
    }

    LedgerEntryWrapper
    getAccount(LedgerHeaderWrapper const& header,
               TransactionFrame const& tx,
               AccountID const& accountID) const override
    {
        return getAccount(accountID);
    }

    LedgerEntryWrapper
    load(LedgerKey const& key) const override
    {
        auto it = mOverlay->find(key);
        if (it != mOverlay->end())
        {
            return LedgerEntryWrapper(it->second);
        }
        return mInner.load(key);
    }

    void
    executeWithMaybeInnerSnapshot(
        std::function<void(LedgerSnapshot const&)> f) const override
    {
        // Pre-V_8 nested-snapshot path is not relevant to Soroban
        // (V_20+); delegate to the inner bucket snapshot, which
        // already documents this as illegal.
        mInner.executeWithMaybeInnerSnapshot(f);
    }
};

std::vector<uint32_t>
getModuleCacheProtocols()
{
    std::vector<uint32_t> ledgerVersions;
    for (uint32_t i = (uint32_t)REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION;
         i <= Config::CURRENT_LEDGER_PROTOCOL_VERSION; i++)
    {
        ledgerVersions.push_back(i);
    }
    auto extra = getenv("SOROBAN_TEST_EXTRA_PROTOCOL");
    if (extra)
    {
        uint32_t proto = static_cast<uint32_t>(atoi(extra));
        if (proto > 0)
        {
            ledgerVersions.push_back(proto);
        }
    }
    return ledgerVersions;
}

void
setLedgerTxnHeader(LedgerHeader const& lh, Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    ltx.loadHeader().current() = lh;
    ltx.commit();
}

bool
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
}

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

LedgerManagerImpl::LedgerApplyMetrics::LedgerApplyMetrics(
    MetricsRegistry& registry)
    : mSorobanMetrics(registry)
    , mTransactionApply(registry.NewTimer({"ledger", "transaction", "apply"}))
    , mTotalTxApply(registry.NewTimer({"ledger", "transaction", "total-apply"}))
    , mTransactionCount(
          registry.NewHistogram({"ledger", "transaction", "count"}))
    , mOperationCount(registry.NewHistogram({"ledger", "operation", "count"}))
    , mPrefetchHitRate(
          registry.NewHistogram({"ledger", "prefetch", "hit-rate"}))
    , mLedgerClose(registry.NewTimer({"ledger", "ledger", "close"}))
    , mLedgerAgeClosed(registry.NewBuckets({"ledger", "age", "closed"},
                                           {5000.0, 7000.0, 10000.0, 20000.0}))
    , mLedgerAge(registry.NewCounter({"ledger", "age", "current-seconds"}))
    , mTransactionApplySucceeded(
          registry.NewCounter({"ledger", "apply", "success"}))
    , mTransactionApplyFailed(
          registry.NewCounter({"ledger", "apply", "failure"}))
    , mSorobanTransactionApplySucceeded(
          registry.NewCounter({"ledger", "apply-soroban", "success"}))
    , mSorobanTransactionApplyFailed(
          registry.NewCounter({"ledger", "apply-soroban", "failure"}))
    , mMaxClustersPerLedger(
          registry.NewCounter({"ledger", "apply-soroban", "max-clusters"}))
    , mStagesPerLedger(
          registry.NewCounter({"ledger", "apply-soroban", "stages"}))
    , mMetaStreamBytes(
          registry.NewMeter({"ledger", "metastream", "bytes"}, "byte"))
    , mMetaStreamWriteTime(registry.NewTimer({"ledger", "metastream", "write"}))
{
}

LedgerManagerImpl::ApplyState::ApplyState(Application& app)
    : mMetrics(app.getMetrics())
    , mAppConnector(app.getAppConnector())
    , mModuleCache(::rust_bridge::new_module_cache())
    , mModuleCacheProtocols(getModuleCacheProtocols())
    , mNumCompilationThreads(app.getConfig().COMPILATION_THREADS)
{
}

LedgerManagerImpl::LedgerApplyMetrics&
LedgerManagerImpl::ApplyState::getMetrics()
{
    return mMetrics;
}

InMemorySorobanState const&
LedgerManagerImpl::ApplyState::getInMemorySorobanState() const
{
    releaseAssert(mPhase == Phase::APPLYING ||
                  mPhase == Phase::SETTING_UP_STATE ||
                  mPhase == Phase::READY_TO_APPLY);
    return mInMemorySorobanState;
}

InMemorySorobanState&
LedgerManagerImpl::ApplyState::getInMemorySorobanStateForUpdate()
{
    // C9: post-Rust-apply, the canonical InMemorySorobanState is mutated
    // from inside apply_soroban_phase, which runs during the APPLYING
    // phase. The Rust orchestrator only mutates the state single-
    // threaded after its worker scope joins, so the "APPLYING means
    // immutable to C++ readers" invariant still holds for any
    // concurrent C++ access — but the update itself happens here.
    releaseAssert(mPhase == Phase::SETTING_UP_STATE ||
                  mPhase == Phase::APPLYING ||
                  mPhase == Phase::COMMITTING);
    return mInMemorySorobanState;
}

#ifdef BUILD_TESTS
InMemorySorobanState&
LedgerManagerImpl::ApplyState::getInMemorySorobanStateForTesting()
{
    return mInMemorySorobanState;
}

::rust::Box<rust_bridge::SorobanModuleCache> const&
LedgerManagerImpl::ApplyState::getModuleCacheForTesting()
{
    return mModuleCache;
}

uint64_t
LedgerManagerImpl::ApplyState::getSorobanInMemoryStateSizeForTesting() const
{
    return mInMemorySorobanState.getSize();
}

void
LedgerManagerImpl::ApplyState::setLedgerStateForTesting(
    CompleteConstLedgerStatePtr state)
{
    mLedgerState = std::move(state);
}
#endif

void
LedgerManagerImpl::ApplyState::threadInvariant() const
{
    if (mAppConnector.getConfig().parallelLedgerClose())
    {
        releaseAssert(threadIsMain() || mAppConnector.threadIsType(
                                            Application::ThreadType::APPLY));
    }
    else
    {
        releaseAssert(threadIsMain());
    }
}

::rust::Box<rust_bridge::SorobanModuleCache> const&
LedgerManagerImpl::ApplyState::getModuleCache() const
{
    releaseAssert(mPhase == Phase::APPLYING);
    return mModuleCache;
}

void
LedgerManagerImpl::markApplyStateReset()
{
    mApplyState.resetToSetupPhase();
}

bool
LedgerManagerImpl::ApplyState::isCompilationRunning() const
{
    return static_cast<bool>(mCompiler);
}

void
LedgerManagerImpl::ApplyState::updateInMemorySorobanState(
    std::vector<LedgerEntry> const& initEntries,
    std::vector<LedgerEntry> const& liveEntries,
    std::vector<LedgerKey> const& deadEntries, LedgerHeader const& lh,
    std::optional<SorobanNetworkConfig const> const& sorobanConfig)
{
    assertWritablePhase();
    // Per-ledger Soroban state updates from the apply phase are
    // applied inside the Rust apply orchestrator. This entry point is
    // for the BucketTestUtils replay path that bypasses apply
    // entirely (setNextLedgerEntryBatchForBucketTesting writes test
    // entries straight into the live BucketList via addLiveBatch);
    // those entries still need to land in SorobanState so that
    // post-apply machinery (eviction scan / LedgerTxn lookups for
    // Soroban keys) sees a consistent view.
    if (!protocolVersionStartsFrom(lh.ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        return;
    }
    rust::Vec<CxxBuf> initBufs;
    initBufs.reserve(initEntries.size());
    for (auto const& e : initEntries)
    {
        initBufs.push_back(toCxxBuf(e));
    }
    rust::Vec<CxxBuf> liveBufs;
    liveBufs.reserve(liveEntries.size());
    for (auto const& e : liveEntries)
    {
        liveBufs.push_back(toCxxBuf(e));
    }
    rust::Vec<CxxBuf> deadBufs;
    deadBufs.reserve(deadEntries.size());
    for (auto const& k : deadEntries)
    {
        deadBufs.push_back(toCxxBuf(k));
    }
    // sorobanConfig provides the cost params used to size CONTRACT_CODE
    // for rent. Required when there are CONTRACT_CODE init / live entries
    // on protocol >= 23; tolerated as nullopt only when the entries are
    // pure data + TTL (e.g. early Soroban tests). Build empty CxxBufs
    // for the cost params in that case — they're only consulted by
    // compute_contract_code_size_for_rent inside batch_update_xdr.
    auto cpu = sorobanConfig.has_value() ? toCxxBuf(sorobanConfig->cpuCostParams())
                                         : CxxBuf{std::make_unique<std::vector<uint8_t>>()};
    auto mem = sorobanConfig.has_value() ? toCxxBuf(sorobanConfig->memCostParams())
                                         : CxxBuf{std::make_unique<std::vector<uint8_t>>()};
    mInMemorySorobanState.getRustStateForBridge()->batch_update_xdr(
        initBufs, liveBufs, deadBufs, lh.ledgerSeq, lh.ledgerVersion,
        Config::CURRENT_LEDGER_PROTOCOL_VERSION, cpu, mem);
}

uint64_t
LedgerManagerImpl::ApplyState::getSorobanInMemoryStateSize() const
{
    // This assert is not strictly necessary, but we don't really want to
    // access the state size outside of the snapshotting process during the
    // LEDGER_CLOSE or SETTING_UP_STATE phase.
    assertWritablePhase();
    return mInMemorySorobanState.getSize();
}

void
LedgerManagerImpl::ApplyState::manuallyAdvanceLedgerHeader(
    LedgerHeader const& lh)
{
    assertCommittingPhase();
    mInMemorySorobanState.manuallyAdvanceLedgerHeader(lh);
}

LedgerManagerImpl::LedgerManagerImpl(Application& app)
    : mApp(app)
    , mApplyState(app)
    , mNumHistoricalSnapshots(app.getConfig().QUERY_SNAPSHOT_LEDGERS)
    , mLastClose(mApp.getClock().now())
    , mCatchupDuration(
          app.getMetrics().NewTimer({"ledger", "catchup", "duration"}))
    , mState(LM_BOOTING_STATE)
{
    // At this point, we haven't called assumeState yet, so the BucketLists are
    // empty. We will create an "empty" snapshot that is not null, but
    // references this empty BucketList (and a ledger header with ledgerSeq 0
    // and zero hash).
    auto& bm = mApp.getBucketManager();
    LedgerHeaderHistoryEntry emptyLcl;
    HistoryArchiveState emptyHas;

    auto initialState = std::make_shared<CompleteConstLedgerState>(
        bm.getLiveBucketList(), bm.getHotArchiveBucketList(), emptyLcl,
        emptyHas, /*sorobanConfig*/ std::nullopt, /*prevState*/ nullptr,
        mNumHistoricalSnapshots);

    mApplyState.setLedgerState(initialState);
    {
        SharedLockExclusive lock(mLedgerStateSnapshotMutex);
        mLastClosedLedgerState = initialState;
    }

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
    releaseAssert(threadIsMain());

    // Go into "applying" state, this will prevent catchup from starting
    mCurrentlyApplyingLedger = true;
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
    static std::array<char const*, LM_NUM_STATE> stateStrings = std::array{
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
    mApplyState.assertSetupPhase();
    auto ledgerTime = mApplyState.getMetrics().mLedgerClose.TimeScope();
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

#ifdef BUILD_TESTS
    // If test account creation is enabled, create additional accounts
    if (cfg.GENESIS_TEST_ACCOUNT_COUNT > 0)
    {
        CLOG_INFO(Ledger, "Creating {} test accounts for genesis ledger",
                  cfg.GENESIS_TEST_ACCOUNT_COUNT);

        // Split totalCoins evenly among all accounts (root + test accounts)
        uint32_t totalAccounts =
            cfg.GENESIS_TEST_ACCOUNT_COUNT + 1; // +1 for root account
        int64_t baseAccountBalance = genesisLedger.totalCoins / totalAccounts;
        int64_t remainder = genesisLedger.totalCoins % totalAccounts;

        // Set root account balance to equal share plus any remainder
        // to ensure we don't lose any coins due to rounding
        rootAccount.balance = baseAccountBalance + remainder;

        // Create accounts using similar approach as TxGenerator::createAccounts
        for (uint32_t i = 0; i < cfg.GENESIS_TEST_ACCOUNT_COUNT; i++)
        {
            auto name = "TestAccount-" + std::to_string(i);
            auto account = txtest::getAccount(name.c_str());

            LedgerEntry testEntry;
            testEntry.lastModifiedLedgerSeq = 1;
            testEntry.data.type(ACCOUNT);
            auto& testAccount = testEntry.data.account();
            testAccount.accountID = account.getPublicKey();
            testAccount.thresholds[0] = 1;
            testAccount.balance = baseAccountBalance;
            ltx.create(testEntry);
        }
    }
#endif

    ltx.create(rootEntry);

    CLOG_INFO(Ledger, "Established genesis ledger, closing");
    CLOG_INFO(Ledger, "Root account: {}", skey.getStrKeyPublic());
    CLOG_INFO(Ledger, "Root account seed: {}", skey.getStrKeySeed().value);

    ApplyLedgerStateSnapshot snap = [this] {
        SharedLockShared guard(mLedgerStateSnapshotMutex);
        return ApplyLedgerStateSnapshot(mLastClosedLedgerState,
                                        mApp.getMetrics());
    }();
    auto output =
        sealLedgerTxnAndStoreInBucketsAndDB(snap, ltx,
                                            /*ledgerCloseMeta*/ nullptr,
                                            /*initialLedgerVers*/ 0);
    advanceLastClosedLedgerState(output);

    ltx.commit();

    // Note: We're still not done with LedgerManager initialization here, as we
    // still need to call setLastClosedLedger to properly initialize
    // LedgerManager after creating the genesis ledger.
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

void
LedgerManagerImpl::loadLastKnownLedgerInternal(bool skipBuildingFullState)
{
    ZoneScoped;
    mApplyState.assertSetupPhase();
    auto rebuildStart = mApp.getClock().now();

    // Step 1. Load LCL state from the DB
    HistoryArchiveState has;
    has.fromString(mApp.getPersistentState().getState(
        PersistentState::kHistoryArchiveState,
        mApp.getDatabase().getSession()));

    // Step 2. Restore LedgerHeader from storestate
    std::optional<LedgerHeader> latestLedgerHeader;
    std::string headerEncoded = mApp.getPersistentState().getState(
        PersistentState::kLastClosedLedgerHeader,
        mApp.getDatabase().getSession());
    if (headerEncoded.empty())
    {
        throw std::runtime_error("Could not load ledger header from database");
    }
    auto currentLedger = std::make_shared<LedgerHeader>(
        LedgerHeaderUtils::decodeFromData(headerEncoded));

    if (currentLedger->ledgerSeq != has.currentLedger)
    {
        throw std::runtime_error("Invalid database state: last known "
                                 "ledger does not agree with HAS");
    }

    CLOG_INFO(Ledger, "Loaded LCL header from database: {}",
              ledgerAbbrev(*currentLedger));
    setLedgerTxnHeader(*currentLedger, mApp);
    latestLedgerHeader = *currentLedger;

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

    // Only restart merges in full startup mode. Many modes in core
    // (standalone offline commands, in-memory setup) do not need to
    // spin up expensive merge processes.
    auto assumeStart = mApp.getClock().now();
    auto assumeStateWork = mApp.getWorkScheduler().executeWork<AssumeStateWork>(
        has, latestLedgerHeader->ledgerVersion,
        /* restartMerges */ !skipBuildingFullState);
    if (assumeStateWork->getState() == BasicWork::State::WORK_SUCCESS)
    {
        std::chrono::duration<double> assumeSecs =
            mApp.getClock().now() - assumeStart;
        CLOG_INFO(Ledger, "Assumed bucket-state for LCL: {} ({:.3f} sec)",
                  ledgerAbbrev(*latestLedgerHeader), assumeSecs.count());
    }
    else
    {
        // Work should only fail during graceful shutdown
        releaseAssertOrThrow(mApp.isStopping());
    }

    // Step 4. Restore LedgerManager's LCL state
    advanceLastClosedLedgerState(advanceApplySnapshotAndMakeLedgerState(
        *latestLedgerHeader, has, /* sorobanConfig */ std::nullopt));

    // Maybe truncate checkpoint files if we're restarting after a crash
    // in applyLedger (in which case any modifications to the ledger state have
    // been rolled back)
    mApp.getHistoryManager().restoreCheckpoint(latestLedgerHeader->ledgerSeq);

    // Prime module cache using mApplyState, which at this point contains LCL
    // state. This is acceptable because we just started and there is no apply
    // thread running yet.
    if (!skipBuildingFullState)
    {
        mApplyState.compileAllContractsInLedger(
            latestLedgerHeader->ledgerVersion);

        auto populateStart = mApp.getClock().now();
        CLOG_INFO(Perf,
                  "Populating in-memory Soroban state from LCL for ledger {}",
                  ledgerAbbrev(*latestLedgerHeader));
        mApplyState.populateInMemorySorobanState();
        std::chrono::duration<double> populateSecs =
            mApp.getClock().now() - populateStart;
        CLOG_INFO(Perf, "Populated in-memory Soroban state in {:.3f} sec",
                  populateSecs.count());

        maybeRunSnapshotInvariantFromLedgerState(copyApplyLedgerStateSnapshot(),
                                                 /* runInParallel */ false);
    }
    mApplyState.markEndOfSetupPhase();

    std::chrono::duration<double> rebuildSecs =
        mApp.getClock().now() - rebuildStart;
    CLOG_INFO(Perf, "Startup state load took {:.3f} sec (full={})",
              rebuildSecs.count(), !skipBuildingFullState);
}

void
LedgerManagerImpl::loadLastKnownLedger()
{
    loadLastKnownLedgerInternal(/* skipBuildingFullState */ false);
}

void
LedgerManagerImpl::partiallyLoadLastKnownLedgerForUtils()
{
    loadLastKnownLedgerInternal(/* skipBuildingFullState */ true);
}

Database&
LedgerManagerImpl::getDatabase()
{
    return mApp.getDatabase();
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSize() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->getLastClosedLedgerHeader()
        .header.maxTxSetSize;
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSizeOps() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    auto n =
        mLastClosedLedgerState->getLastClosedLedgerHeader().header.maxTxSetSize;
    return protocolVersionStartsFrom(
               mLastClosedLedgerState->getLastClosedLedgerHeader()
                   .header.ledgerVersion,
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
        return getLastClosedSorobanNetworkConfig().maxLedgerResources();
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
        mApp.getLedgerManager().getLastClosedSorobanNetworkConfig();
    int64_t const opCount = 1;
    std::vector<int64_t> limits = {opCount,
                                   conf.txMaxInstructions(),
                                   conf.txMaxSizeBytes(),
                                   conf.txMaxDiskReadBytes(),
                                   conf.txMaxWriteBytes(),
                                   conf.txMaxDiskReadEntries(),
                                   conf.txMaxWriteLedgerEntries()};
    return Resource(limits);
}

int64_t
LedgerManagerImpl::getLastMinBalance(uint32_t ownerCount) const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    auto const& lh = mLastClosedLedgerState->getLastClosedLedgerHeader().header;
    if (protocolVersionIsBefore(lh.ledgerVersion, ProtocolVersion::V_9))
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2LL + ownerCount) * int64_t(lh.baseReserve);
}

uint32_t
LedgerManagerImpl::getLastReserve() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->getLastClosedLedgerHeader()
        .header.baseReserve;
}

uint32_t
LedgerManagerImpl::getLastTxFee() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->getLastClosedLedgerHeader().header.baseFee;
}

LedgerHeaderHistoryEntry const&
LedgerManagerImpl::getLastClosedLedgerHeader() const
{
    // Must be main thread: returns a reference into mLastClosedLedgerState,
    // which is only replaced on the main thread (advanceLastClosedLedgerState).
    // A cross-thread caller could hold a dangling reference after replacement.
    releaseAssert(threadIsMain());
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->getLastClosedLedgerHeader();
}

HistoryArchiveState
LedgerManagerImpl::getLastClosedLedgerHAS() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->getLastClosedHistoryArchiveState();
}

uint32_t
LedgerManagerImpl::getLastClosedLedgerNum() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->getLastClosedLedgerHeader().header.ledgerSeq;
}

void
LedgerManagerImpl::maybeRunSnapshotInvariantFromLedgerState(
    ApplyLedgerStateSnapshot const& ledgerState, bool runInParallel)
{
    if (!mApp.getConfig().INVARIANT_EXTRA_CHECKS || mApp.isStopping() ||
        !mApp.getInvariantManager().shouldRunInvariantSnapshot())
    {
        return;
    }

    // TODO(C14b): the snapshot-invariant path used to deep-copy
    // InMemorySorobanState into a shared_ptr to hand off to a background
    // thread. After the C++ shim refactor, InMemorySorobanState is
    // non-copyable (the Rust state would need a clone bridge that we
    // don't yet expose, and the use case is invariant-only). The state-
    // snapshot invariant is disabled in this window. Re-enabled in C14b
    // via the public read API.
    (void)ledgerState;
    (void)runInParallel;
}

SorobanNetworkConfig const&
LedgerManagerImpl::getLastClosedSorobanNetworkConfig() const
{
    // Must be main thread: returns a reference into mLastClosedLedgerState,
    // which is only replaced on the main thread (advanceLastClosedLedgerState).
    // A cross-thread caller could hold a dangling reference after replacement.
    releaseAssert(threadIsMain());
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    releaseAssert(mLastClosedLedgerState->hasSorobanConfig());
    return mLastClosedLedgerState->getSorobanConfig();
}

bool
LedgerManagerImpl::hasLastClosedSorobanNetworkConfig() const
{
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return mLastClosedLedgerState->hasSorobanConfig();
}

std::chrono::milliseconds
LedgerManagerImpl::getExpectedLedgerCloseTime() const
{
    releaseAssert(threadIsMain());

    auto const& cfg = mApp.getConfig();
    if (auto overrideOp = cfg.getExpectedLedgerCloseTimeTestingOverride();
        overrideOp.has_value())
    {
        return *overrideOp;
    }

    auto const& lcl = getLastClosedLedgerHeader();
    if (protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  ProtocolVersion::V_23))
    {
        auto const& networkConfig = getLastClosedSorobanNetworkConfig();
        return std::chrono::milliseconds(
            networkConfig.ledgerTargetCloseTimeMilliseconds());
    }

    return Herder::TARGET_LEDGER_CLOSE_TIME_BEFORE_PROTOCOL_VERSION_23_MS;
}

#ifdef BUILD_TESTS
LedgerManagerImpl::LedgerClosePhaseTimings const&
LedgerManagerImpl::getLastPhaseTimings() const
{
    return mLastPhaseTimings;
}

std::vector<TransactionMetaFrame> const&
LedgerManagerImpl::getLastClosedLedgerTxMeta()
{
    return mLastLedgerTxMeta;
}

std::optional<LedgerCloseMetaFrame> const&
LedgerManagerImpl::getLastClosedLedgerCloseMeta()
{
    return mLastLedgerCloseMeta;
}

void
LedgerManagerImpl::storeCurrentLedgerForTest(LedgerHeader const& header)
{
    storePersistentStateAndLedgerHeaderInDB(header, true);
}

InMemorySorobanState const&
LedgerManagerImpl::getInMemorySorobanStateForTesting()
{
    return mApplyState.getInMemorySorobanStateForTesting();
}

void
LedgerManagerImpl::rebuildInMemorySorobanStateForTesting(uint32_t ledgerVersion)
{
    mApplyState.resetToSetupPhase();
    mApplyState.getInMemorySorobanStateForTesting().clearForTesting();
    mApplyState.populateInMemorySorobanState();
    mApplyState.markEndOfSetupPhase();
}

::rust::Box<rust_bridge::SorobanModuleCache>
LedgerManagerImpl::getModuleCacheForTesting()
{
    releaseAssert(!mApplyState.isCompilationRunning());
    return mApplyState.getModuleCacheForTesting()->shallow_clone();
}

uint64_t
LedgerManagerImpl::getSorobanInMemoryStateSizeForTesting()
{
    return mApplyState.getSorobanInMemoryStateSizeForTesting();
}
#endif

SorobanMetrics&
LedgerManagerImpl::getSorobanMetrics()
{
    return mApplyState.getMetrics().mSorobanMetrics;
}

std::unique_ptr<LedgerTxnRoot>
LedgerManagerImpl::createLedgerTxnRoot(Application& app, size_t entryCacheSize,
                                       size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                                       ,
                                       bool bestOfferDebuggingEnabled
#endif
)
{
    return std::make_unique<LedgerTxnRoot>(
        app, mApplyState.getInMemorySorobanState(), entryCacheSize,
        prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
        ,
        bestOfferDebuggingEnabled
#endif
    );
}

::rust::Box<rust_bridge::SorobanModuleCache>
LedgerManagerImpl::getModuleCache()
{
    // There should not be any compilation running when
    // anyone calls this function. It is accessed from
    // transactions during apply only.
    releaseAssert(!mApplyState.isCompilationRunning());
    return mApplyState.getModuleCache()->shallow_clone();
}

void
LedgerManagerImpl::handleUpgradeAffectingSorobanInMemoryStateSize(
    AbstractLedgerTxn& upgradeLtx)
{
    mApplyState.handleUpgradeAffectingSorobanInMemoryStateSize(upgradeLtx);
}

void
LedgerManagerImpl::ApplyState::handleUpgradeAffectingSorobanInMemoryStateSize(
    AbstractLedgerTxn& upgradeLtx)
{
    assertCommittingPhase();

    // Load the current network from the ledger. It might be in some
    // intermediate state, which is fine, because we call this only after
    // a relevant section has been upgraded and all the remaining sections
    // are not relevant for the size computation.
    auto currentConfig = SorobanNetworkConfig::loadFromLedger(upgradeLtx);
    auto upgradeLedgerVersion = upgradeLtx.loadHeader().current().ledgerVersion;
    mInMemorySorobanState.recomputeContractCodeSize(currentConfig,
                                                    upgradeLedgerVersion);

    // We need to record the updated size, but only when we're in p23+, as
    // before that we store BL size instead.
    if (protocolVersionStartsFrom(upgradeLedgerVersion, ProtocolVersion::V_23))
    {
        SorobanNetworkConfig::updateRecomputedSorobanStateSize(
            mInMemorySorobanState.getSize(), upgradeLtx);
    }
}

void
LedgerManagerImpl::ApplyState::finishPendingCompilation()
{
    assertWritablePhase();
    releaseAssert(mCompiler);
    auto newCache = mCompiler->wait();
    getMetrics().mSorobanMetrics.mModuleCacheRebuildBytes.set_count(
        (int64)mCompiler->getBytesCompiled());
    getMetrics().mSorobanMetrics.mModuleCacheNumEntries.set_count(
        (int64)mCompiler->getContractsCompiled());
    getMetrics().mSorobanMetrics.mModuleCacheRebuildTime.Update(
        mCompiler->getCompileTime());
    mModuleCache.swap(newCache);
    mCompiler.reset();
}

void
LedgerManagerImpl::ApplyState::compileAllContractsInLedger(
    uint32_t minLedgerVersion)
{
    assertSetupPhase();
    startCompilingAllContracts(minLedgerVersion);
    finishPendingCompilation();
}

void
LedgerManagerImpl::ApplyState::populateInMemorySorobanState()
{
    assertSetupPhase();

    // Enumerate live-bucket file paths in priority order: for each level
    // 0..kNumLevels-1, take curr first, then snap. The Rust side walks
    // these in order, treating earlier paths as higher-priority (newer)
    // sources for dedup against DEADENTRY records. Empty bucket files are
    // skipped (their corresponding shared_ptr is non-null but isEmpty).
    auto& bm = mAppConnector.getBucketManager();
    auto& bl = bm.getLiveBucketList();
    std::vector<std::string> paths;
    paths.reserve(LiveBucketList::kNumLevels * 2);
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        if (auto curr = level.getCurr(); curr && !curr->isEmpty())
        {
            paths.push_back(curr->getFilename().string());
        }
        if (auto snap = level.getSnap(); snap && !snap->isEmpty())
        {
            paths.push_back(snap->getFilename().string());
        }
    }

    auto const& lh = mLedgerState->getLastClosedLedgerHeader().header;
    std::optional<SorobanNetworkConfig const> config;
    if (mLedgerState->hasSorobanConfig())
    {
        // optional<T const>::operator= is deleted; emplace constructs in place.
        config.emplace(mLedgerState->getSorobanConfig());
    }
    mInMemorySorobanState.initializeFromBucketFiles(paths, lh.ledgerSeq,
                                                    lh.ledgerVersion, config);
}

void
LedgerManagerImpl::ApplyState::assertCommittingPhase() const
{
    threadInvariant();
    releaseAssert(mPhase == Phase::COMMITTING);
}

void
LedgerManagerImpl::ApplyState::markStartOfApplying()
{
    threadInvariant();
    releaseAssert(mPhase == Phase::READY_TO_APPLY);
    mPhase = Phase::APPLYING;
}

void
LedgerManagerImpl::ApplyState::markStartOfCommitting()
{
    threadInvariant();
    releaseAssert(mPhase == Phase::APPLYING);
    mPhase = Phase::COMMITTING;
}

void
LedgerManagerImpl::ApplyState::markEndOfCommitting()
{
    assertCommittingPhase();
    mPhase = Phase::READY_TO_APPLY;
}

void
LedgerManagerImpl::ApplyState::markEndOfSetupPhase()
{
    threadInvariant();
    releaseAssert(mPhase == Phase::SETTING_UP_STATE);
    mPhase = Phase::READY_TO_APPLY;
}

void
LedgerManagerImpl::ApplyState::resetToSetupPhase()
{
    threadInvariant();
    releaseAssert(mPhase == Phase::READY_TO_APPLY);
    mPhase = Phase::SETTING_UP_STATE;
}

void
LedgerManagerImpl::ApplyState::assertSetupPhase() const
{
    threadInvariant();
    releaseAssert(mPhase == Phase::SETTING_UP_STATE);
}

void
LedgerManagerImpl::ApplyState::startCompilingAllContracts(
    uint32_t minLedgerVersion)
{
    threadInvariant();
    // Always stop a previous compilation before starting a new one. Can only
    // have one running at any time.
    releaseAssert(!mCompiler);
    std::vector<uint32_t> versions;
    for (auto const& v : mModuleCacheProtocols)
    {
        if (v >= minLedgerVersion)
        {
            versions.push_back(v);
        }
    }
    mCompiler = std::make_unique<SharedModuleCacheCompiler>(
        copyLedgerStateSnapshot(), mNumCompilationThreads, versions);
    mCompiler->start();
}

void
LedgerManagerImpl::ApplyState::assertWritablePhase() const
{
    threadInvariant();
    releaseAssert(mPhase == Phase::SETTING_UP_STATE ||
                  mPhase == Phase::COMMITTING);
}

void
LedgerManagerImpl::ApplyState::maybeRebuildModuleCache(
    uint32_t minLedgerVersion)
{
    assertCommittingPhase();
    auto snap = copyLedgerStateSnapshot();

    // There is (currently) a grow-only arena underlying the module cache, so as
    // entries are uploaded and evicted that arena will still grow. To cap this
    // growth, we periodically rebuild the module cache from scratch.
    //
    // We could pick various size caps, but we want to avoid rebuilding
    // spuriously when there just happens to be "a fairly large" cache due to
    // having a fairly large live BL. I.e. we want to allow it to get as big as
    // we can -- or as big as the "natural" BL-limits-dictated size -- while
    // still rebuilding fairly often in DoS-attempt scenarios or just generally
    // if there's regular upload/expiry churn that would otherwise cause
    // unbounded growth.
    //
    // Unfortunately we do not know exactly how much memory is used by each byte
    // of contract we compile, and the size estimates from the cost model have
    // to assume a worst case which is almost a factor of _40_ larger than the
    // byte-size of the contracts. So for example if we assume 100MB of
    // contracts, the cost model says we ought to budget for 4GB of memory, just
    // in case _all 100MB of contracts_ are "the worst case contract" that's
    // just a continuous stream of function definitions.
    //
    // So: we take this multiplier, times the size of the contracts we _last_
    // drew from the BL when doing a full recompile, times two, as a cap on the
    // _current_ (post-rebuild, currently-growing) cache's budget-tracked
    // memory. This should avoid rebuilding spuriously, while still treating
    // events that double the size of the contract-set in the live BL as an
    // event that warrants a rebuild.

    // We try to fish the current cost multiplier out of the soroban network
    // config's memory cost model, but fall back to a conservative default in
    // case there is no mem cost param for VmInstantiation (This should never
    // happen but just in case).
    uint64_t linearTerm = 5000;

    // linearTerm is in 1/128ths in the cost model, to reduce rounding error.
    uint64_t scale = 128;
    LedgerSnapshot lsForConfig(snap);
    auto sorobanConfig = SorobanNetworkConfig::loadFromLedger(lsForConfig);
    auto const& memParams = sorobanConfig.memCostParams();
    if (memParams.size() > (size_t)stellar::VmInstantiation)
    {
        auto const& param = memParams[(size_t)stellar::VmInstantiation];
        linearTerm = param.linearTerm;
    }
    auto lastBytesCompiled =
        getMetrics().mSorobanMetrics.mModuleCacheRebuildBytes.count();
    uint64_t limit = 2 * lastBytesCompiled * linearTerm / scale;

    for (auto const& v : mModuleCacheProtocols)
    {
        auto bytesConsumed = mModuleCache->get_mem_bytes_consumed(v);
        if (bytesConsumed > limit)
        {
            CLOG_DEBUG(Ledger,
                       "Rebuilding module cache: worst-case estimate {} "
                       "model-bytes consumed of {} limit",
                       bytesConsumed, limit);
            startCompilingAllContracts(minLedgerVersion);
            break;
        }
    }
}

void
LedgerManagerImpl::publishSorobanMetrics()
{
    if (!hasLastClosedSorobanNetworkConfig())
    {
        return;
    }
    auto const& conf = getLastClosedSorobanNetworkConfig();
    auto& m = getSorobanMetrics();
    // first publish the network config limits
    m.mConfigContractDataKeySizeBytes.set_count(
        conf.maxContractDataKeySizeBytes());
    m.mConfigMaxContractDataEntrySizeBytes.set_count(
        conf.maxContractDataEntrySizeBytes());
    m.mConfigMaxContractSizeBytes.set_count(conf.maxContractSizeBytes());
    m.mConfigTxMaxSizeByte.set_count(conf.txMaxSizeBytes());
    m.mConfigTxMaxCpuInsn.set_count(conf.txMaxInstructions());
    m.mConfigTxMemoryLimitBytes.set_count(conf.txMemoryLimit());
    m.mConfigTxMaxDiskReadEntries.set_count(conf.txMaxDiskReadEntries());
    m.mConfigTxMaxDiskReadBytes.set_count(conf.txMaxDiskReadBytes());
    m.mConfigTxMaxWriteLedgerEntries.set_count(conf.txMaxWriteLedgerEntries());
    m.mConfigTxMaxWriteBytes.set_count(conf.txMaxWriteBytes());
    m.mConfigMaxContractEventsSizeBytes.set_count(
        conf.txMaxContractEventsSizeBytes());
    m.mConfigLedgerMaxTxCount.set_count(conf.ledgerMaxTxCount());
    m.mConfigLedgerMaxInstructions.set_count(conf.ledgerMaxInstructions());
    m.mConfigLedgerMaxTxsSizeByte.set_count(
        conf.ledgerMaxTransactionSizesBytes());
    m.mConfigLedgerMaxDiskReadEntries.set_count(
        conf.ledgerMaxDiskReadEntries());
    m.mConfigLedgerMaxDiskReadBytes.set_count(conf.ledgerMaxDiskReadBytes());
    m.mConfigLedgerMaxWriteEntries.set_count(
        conf.ledgerMaxWriteLedgerEntries());
    m.mConfigLedgerMaxWriteBytes.set_count(conf.ledgerMaxWriteBytes());
    m.mConfigBucketListTargetSizeByte.set_count(
        conf.sorobanStateTargetSizeBytes());
    m.mConfigFeeWrite1KB.set_count(conf.feeRent1KB());

    // then publish the actual ledger usage
    m.publishAndResetLedgerWideMetrics();
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
LedgerManagerImpl::startCatchup(CatchupConfiguration configuration,
                                std::shared_ptr<HistoryArchive> archive)
{
    ZoneScoped;
    setState(LM_CATCHING_UP_STATE);
    mApp.getLedgerApplyManager().startCatchup(configuration, archive);
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
    mApplyState.getMetrics().mLedgerAge.set_count(
        secondsSinceLastLedgerClose());
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
    auto streamWrite =
        mApplyState.getMetrics().mMetaStreamWriteTime.TimeScope();
    if (mMetaStream)
    {
        size_t written = 0;
        mMetaStream->writeOne(mNextMetaToEmit->getXDR(), nullptr, &written);
        mMetaStream->flush();
        mApplyState.getMetrics().mMetaStreamBytes.Mark(written);
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

namespace
{
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
                    getGlobalRandomEngine())];
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

// Append `key`/`entry` (XDR-serialized) onto a rust::Vec<LedgerEntryInput>
// for the prefetch path the bridge consumes. Encoded bytes move directly
// into a CxxBuf-wrapped unique_ptr<std::vector<uint8_t>> — no per-byte
// push_back loop into a `rust::Vec<u8>`.
void
appendPrefetchEntry(rust::Vec<LedgerEntryInput>& dst, LedgerKey const& key,
                    LedgerEntry const& entry)
{
    LedgerEntryInput u;
    u.key_xdr.data =
        std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(key));
    u.value_xdr.data =
        std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(entry));
    dst.push_back(std::move(u));
}

// Walk every Soroban TX in the phase, collect the union of non-Soroban
// LedgerKeys mentioned in any TX's footprint, deduplicate, load each
// from the LedgerTxn, and pack into the rust::Vec<LedgerEntryInput>
// that apply_soroban_phase consumes as classic_prefetch.
//
// "Non-Soroban" means anything InMemorySorobanState::isInMemoryType
// returns false for — i.e. accounts, trustlines, etc., but not
// CONTRACT_DATA / CONTRACT_CODE / TTL (the latter live in SorobanState).
//
// Keys whose load returns a missing entry are silently omitted; the
// Rust side's layered_get treats a missing classic entry as "skip
// this footprint slot" anyway, matching the C++ apply-path behavior.
rust::Vec<LedgerEntryInput>
buildClassicPrefetchForPhase(AbstractLedgerTxn& ltx,
                             TxSetPhaseFrame const& phase)
{
    // Two-phase build:
    //  1. Sequential walk: for each unique non-Soroban footprint key,
    //     `ltx.loadWithoutRecord` (single-writer ltx — must stay
    //     sequential) and stash the (key, entry) pairs.
    //  2. Parallel XDR encode: `xdr_to_opaque` walks per pair (key +
    //     value tree) — dominant cost for thousand-entry SAC phases.
    //     Fan that across worker threads. Encoded bytes go straight
    //     into LedgerEntryInput's CxxBuf via std::make_unique, no
    //     per-byte push into a rust::Vec<u8>.
    std::vector<std::pair<LedgerKey, LedgerEntry>> loaded;
    UnorderedSet<LedgerKey> seen;
    auto walkFootprint = [&](xdr::xvector<LedgerKey> const& keys) {
        for (auto const& key : keys)
        {
            if (InMemorySorobanState::isInMemoryType(key))
            {
                continue;
            }
            if (!seen.insert(key).second)
            {
                continue;
            }
            auto entry = ltx.loadWithoutRecord(key);
            if (!entry)
            {
                continue;
            }
            loaded.emplace_back(key, entry.current());
        }
    };
    for (auto const& tx : phase)
    {
        if (!tx->isSoroban())
        {
            continue;
        }
        auto const& resources = tx->sorobanResources();
        walkFootprint(resources.footprint.readOnly);
        walkFootprint(resources.footprint.readWrite);
    }

    rust::Vec<LedgerEntryInput> prefetch;
    prefetch.reserve(loaded.size());
    constexpr size_t MIN_PARALLEL = 256;
    if (loaded.size() < MIN_PARALLEL)
    {
        for (auto const& [key, entry] : loaded)
        {
            appendPrefetchEntry(prefetch, key, entry);
        }
        return prefetch;
    }
    constexpr size_t MAX_WORKERS = 8;
    size_t workerCount = std::min<size_t>(
        MAX_WORKERS, std::thread::hardware_concurrency());
    workerCount = std::max<size_t>(1, workerCount);
    workerCount = std::min(workerCount, loaded.size());
    std::vector<LedgerEntryInput> encoded(loaded.size());
    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    size_t baseChunk = loaded.size() / workerCount;
    size_t remainder = loaded.size() % workerCount;
    size_t begin = 0;
    for (size_t w = 0; w < workerCount; ++w)
    {
        size_t chunk = baseChunk + (w < remainder ? 1u : 0u);
        size_t end = begin + chunk;
        threads.emplace_back([&loaded, &encoded, begin, end]() {
            for (size_t i = begin; i < end; ++i)
            {
                auto const& [k, e] = loaded[i];
                LedgerEntryInput u;
                u.key_xdr.data = std::make_unique<std::vector<uint8_t>>(
                    xdr::xdr_to_opaque(k));
                u.value_xdr.data = std::make_unique<std::vector<uint8_t>>(
                    xdr::xdr_to_opaque(e));
                encoded[i] = std::move(u);
            }
        });
        begin = end;
    }
    for (auto& t : threads)
    {
        t.join();
    }
    for (auto& u : encoded)
    {
        prefetch.push_back(std::move(u));
    }
    return prefetch;
}

// Walk every RestoreFootprint TX in the phase, gather the union of
// CONTRACT_DATA / CONTRACT_CODE keys mentioned in any RW footprint,
// deduplicate, and bulk-look them up against the hot-archive snapshot.
// Each key whose archive entry is HOT_ARCHIVE_ARCHIVED becomes a
// (key, archivedEntry) pair in the prefetch vec the bridge passes
// to apply_soroban_phase as archived_prefetch.
//
// Rust's RestoreFootprint driver only consults archived_prefetch after
// the layered live-state lookup has come up empty for a given
// footprint slot, so over-prefetching here (e.g. for keys whose live
// TTL is still valid) is harmless — those entries will simply be
// ignored. We keep the prefetch tight to reduce I/O nonetheless.
//
// Keys whose archive entry is HOT_ARCHIVE_LIVE (resurrected, no longer
// in the archive) or absent are silently skipped; the Rust driver
// handles the no-such-archived-entry case as "skip this footprint
// slot" already.
rust::Vec<LedgerEntryInput>
buildArchivedPrefetchForPhase(ApplyLedgerStateSnapshot const& snap,
                              TxSetPhaseFrame const& phase)
{
    std::set<LedgerKey, LedgerEntryIdCmp> archiveKeys;
    for (auto const& tx : phase)
    {
        if (!tx->isSoroban())
        {
            continue;
        }
        std::optional<OperationType> opType;
        for (auto const& opFrame : tx->getOperationFrames())
        {
            opType = opFrame->getOperation().body.type();
            break;
        }
        if (!opType)
        {
            continue;
        }
        auto const& fp = tx->sorobanResources().footprint;
        // RestoreFootprint TXs need to look up archived RW data/code
        // entries to bring them back into the live BL. InvokeHostFunction
        // TXs need to detect archived footprint entries (RO + RW) so the
        // pre-host walk can fail with ENTRY_ARCHIVED before the host
        // sees a missing entry.
        if (*opType == RESTORE_FOOTPRINT)
        {
            for (auto const& key : fp.readWrite)
            {
                if (key.type() == CONTRACT_DATA ||
                    key.type() == CONTRACT_CODE)
                {
                    archiveKeys.insert(key);
                }
            }
        }
        else if (*opType == INVOKE_HOST_FUNCTION)
        {
            for (auto const& key : fp.readOnly)
            {
                if (key.type() == CONTRACT_DATA ||
                    key.type() == CONTRACT_CODE)
                {
                    archiveKeys.insert(key);
                }
            }
            for (auto const& key : fp.readWrite)
            {
                if (key.type() == CONTRACT_DATA ||
                    key.type() == CONTRACT_CODE)
                {
                    archiveKeys.insert(key);
                }
            }
        }
    }
    rust::Vec<LedgerEntryInput> prefetch;
    if (archiveKeys.empty())
    {
        return prefetch;
    }
    auto archived = snap.loadArchiveKeys(archiveKeys);
    for (auto const& bucketEntry : archived)
    {
        if (bucketEntry.type() != HOT_ARCHIVE_ARCHIVED)
        {
            continue;
        }
        auto const& entry = bucketEntry.archivedEntry();
        appendPrefetchEntry(prefetch, LedgerEntryKey(entry), entry);
    }
    return prefetch;
}
} // namespace

void
LedgerManagerImpl::ledgerCloseComplete(uint32_t lcl, bool calledViaExternalize,
                                       LedgerCloseData const& ledgerData,
                                       bool upgradeApplied)
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

    // Without parallel ledger apply, this should always be true
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
        mApp.getHerder().lastClosedLedgerIncreased(
            appliedLatest, ledgerData.getTxSet(), upgradeApplied);
    }
}

void
LedgerManagerImpl::advanceLedgerStateAndPublish(
    uint32_t ledgerSeq, bool calledViaExternalize,
    LedgerCloseData const& ledgerData,
    CompleteConstLedgerStatePtr newLedgerState, bool upgradeApplied)
{
#ifdef BUILD_TESTS
    if (mAdvanceLedgerStateAndPublishOverride)
    {
        mAdvanceLedgerStateAndPublishOverride();
        return;
    }
#endif

    // Perform LCL->appliedLedgerState transition on the _main_ thread, and kick
    // off publishing, cleanup bucket files, notify herder to trigger next
    // ledger.
    releaseAssert(threadIsMain());
    advanceLastClosedLedgerState(newLedgerState);
    // We can publish Soroban metrics at any point after advancing the LCL
    // state.
    publishSorobanMetrics();

    // Maybe kick off publishing on complete checkpoint files
    auto& hm = mApp.getHistoryManager();
    hm.publishQueuedHistory();
    hm.logAndUpdatePublishStatus();

    // Clean up unreferenced buckets post-apply
    {
        // Ledger state might be updated at the same time, so protect GC
        // call with state mutex
        JITTER_INJECT_DELAY();
        RecursiveMutexLocker lock(mLedgerStateMutex);
        JITTER_INJECT_DELAY();
        mApp.getBucketManager().forgetUnreferencedBuckets(
            getLastClosedLedgerHAS());
    }

    // Maybe set LedgerManager into synced state, maybe let
    // Herder trigger next ledger
    ledgerCloseComplete(ledgerSeq, calledViaExternalize, ledgerData,
                        upgradeApplied);
    CLOG_INFO(Ledger, "Ledger close complete: {}", ledgerSeq);
}

// This is the main entrypoint for the apply thread (and/or synchronous
// application happening on the main thread -- it can happen on either).
// It is called from the LedgerApplyManager and will post its results
// back to the main thread when done, if running on the apply thread.
void
LedgerManagerImpl::applyLedger(LedgerCloseData const& ledgerData,
                               bool calledViaExternalize)
{
    if (mApp.isStopping())
    {
        return;
    }

    JITTER_INJECT_DELAY();

    // Complete any pending wasm-module-compilation before closing the ledger.
    // This might or might-not exist, depending on whether we triggered a
    // compilation in the previous ledger-apply.
    if (mApplyState.isCompilationRunning())
    {
        mApplyState.finishPendingCompilation();
    }

#ifdef BUILD_TESTS
    mLastLedgerTxMeta.clear();
    mLastLedgerCloseMeta.reset();
#endif
    ZoneScoped;
    mApplyState.markStartOfApplying();
    JITTER_INJECT_DELAY();

    auto ledgerTime = mApplyState.getMetrics().mLedgerClose.TimeScope();
    LogSlowExecution applyLedgerTime{"applyLedger",
                                     LogSlowExecution::Mode::MANUAL, "",
                                     std::chrono::milliseconds::max()};

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    // Note: applyLedger should be able to work correctly based on ledger header
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
    CLOG_DEBUG(Ledger, "starting applyLedger() on ledgerSeq={}",
               header.current().ledgerSeq);

    ZoneValue(static_cast<int64_t>(header.current().ledgerSeq));

    auto now = mApp.getClock().now();
    mApplyState.getMetrics().mLedgerAgeClosed.Update(now - mLastClose);
    // mLastClose is only accessed by a single thread, so no synchronization
    // needed
    mLastClose = now;
    mApplyState.getMetrics().mLedgerAge.set_count(0);

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
    JITTER_INJECT_DELAY();

    if (txSet->previousLedgerHash() != prevHash)
    {
        CLOG_ERROR(Ledger, "TxSet mismatch: LCD wants {}, LCL is {}",
                   ledgerAbbrev(ledgerData.getLedgerSeq() - 1,
                                txSet->previousLedgerHash()),
                   ledgerAbbrev(prevHeader));

        CLOG_ERROR(Ledger, "{}", xdrToCerealString(prevHeader, "Full LCL"));
        CLOG_ERROR(Ledger, "{}", POSSIBLY_CORRUPTED_LOCAL_DATA);

#ifdef BUILD_TESTS
        if (!threadIsMain())
        {
            throw std::runtime_error(
                "txset mismatch on background apply thread. This usually means "
                "a test directly modified the LedgerTxnRoot header (e.g. "
                "totalCoins). Set cfg.PARALLEL_LEDGER_APPLY = false for such "
                "tests.");
        }
#endif

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
#ifdef BUILD_TESTS
    auto phaseStart = std::chrono::steady_clock::now();
#endif
    auto applicableTxSet = txSet->prepareForApply(mApp, prevHeader);
#ifdef BUILD_TESTS
    auto phaseEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.prepareTxSetMs =
        std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
            .count();
#endif

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

#ifdef BUILD_TESTS
    // We always store the ledgerCloseMeta in tests so we can inspect it,
    // unless explicitly disabled for benchmarking.
    if (!ledgerCloseMeta && !mApp.getConfig().DISABLE_TX_META_FOR_TESTING)
    {
        ledgerCloseMeta = std::make_unique<LedgerCloseMetaFrame>(
            header.current().ledgerVersion);
        ledgerCloseMeta->reserveTxProcessing(applicableTxSet->sizeTxTotal());
        ledgerCloseMeta->populateTxSet(*txSet);
    }
#endif

    TransactionResultSet txResultSet;
#ifdef BUILD_TESTS
    if (mApp.getRunInOverlayOnlyMode())
    {
        auto numTxs = txSet->sizeTxTotal();
        auto numOps = txSet->sizeOpTotalForLogging();

        // Force-deactivate header in overlay only mode; in normal mode, this is
        // done by `processFeesSeqNums`
        ltx.deactivateHeaderTestOnly();
        mApplyState.getMetrics().mTransactionCount.Update(
            static_cast<int64_t>(numTxs));
        TracyPlot("ledger.transaction.count", static_cast<int64_t>(numTxs));

        mApplyState.getMetrics().mOperationCount.Update(
            static_cast<int64_t>(numOps));
        TracyPlot("ledger.operation.count", static_cast<int64_t>(numOps));
    }
    else
#endif
    {
        // first, prefetch source accounts for txset, then charge fees
#ifdef BUILD_TESTS
        phaseStart = std::chrono::steady_clock::now();
#endif
        prefetchTxSourceIds(mApp.getLedgerTxnRoot(), *applicableTxSet,
                            mApp.getConfig());
#ifdef BUILD_TESTS
        phaseEnd = std::chrono::steady_clock::now();
        mLastPhaseTimings.prefetchSourceAccountsMs =
            std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
                .count();
#endif

        // Time the entire transaction processing phase from fee processing
        // through transaction application
        auto totalTxApplyTime =
            mApplyState.getMetrics().mTotalTxApply.TimeScope();

        // Subtle: after this call, `header` is invalidated, and is not safe
        // to use
#ifdef BUILD_TESTS
        phaseStart = std::chrono::steady_clock::now();
#endif
        auto const mutableTxResults = processFeesSeqNums(
            *applicableTxSet, ltx, ledgerCloseMeta, ledgerData);
#ifdef BUILD_TESTS
        phaseEnd = std::chrono::steady_clock::now();
        mLastPhaseTimings.processFeesSeqNumsMs =
            std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
                .count();
        phaseStart = std::chrono::steady_clock::now();
#endif
        txResultSet = applyTransactions(*applicableTxSet, mutableTxResults, ltx,
                                        ledgerCloseMeta);
#ifdef BUILD_TESTS
        phaseEnd = std::chrono::steady_clock::now();
        mLastPhaseTimings.applyTransactionsMs =
            std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
                .count();
#endif
    }

    if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
    {
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        mApp.getHistoryManager().appendTransactionSet(ledgerSeq, txSet,
                                                      txResultSet);
    }

    ltx.loadHeader().current().txSetResultHash = xdrSha256(txResultSet);

    // apply any upgrades that were decided during consensus
    // this must be done after applying transactions as the txset
    // was validated before upgrades. At this point, we've finished executing
    // transactions and are beginning to commit ledger phase.
    mApplyState.markStartOfCommitting();
    JITTER_INJECT_DELAY();

#ifdef BUILD_TESTS
    phaseStart = std::chrono::steady_clock::now();
#endif
    bool upgradeApplied = false;
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
            upgradeApplied = true;
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
#ifdef BUILD_TESTS
    phaseEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.applyUpgradesMs =
        std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
            .count();
#endif

    auto maybeNewVersion = ltx.loadHeader().current().ledgerVersion;
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;

#ifdef BUILD_TESTS
    phaseStart = std::chrono::steady_clock::now();
#endif
    auto lclSnap = mApplyState.copyLedgerStateSnapshot();
    auto appliedLedgerState = sealLedgerTxnAndStoreInBucketsAndDB(
        lclSnap, ltx, ledgerCloseMeta, initialLedgerVers);
#ifdef BUILD_TESTS
    phaseEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.sealAndBucketMs =
        std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
            .count();
#endif

    // NB: from now on, the ledger state may not change, but LCL still hasn't
    // advanced properly. Hence when requesting the ledger state data (such as
    // Soroban network config or header) we should use the `appliedLedgerState`
    // getters.

    // Meta is still v0 during the protocol 20 upgrade (because it is created
    // *before* we even know that the upgrade happens), so we only update the
    // config-related information if we already were on protocol 20 when the
    // meta was created.
    if (ledgerCloseMeta &&
        protocolVersionStartsFrom(initialLedgerVers, SOROBAN_PROTOCOL_VERSION))
    {
        ledgerCloseMeta->setNetworkConfiguration(
            appliedLedgerState->getSorobanConfig(),
            mApp.getConfig().EMIT_LEDGER_CLOSE_META_EXT_V1);
    }

    if (ledgerData.getExpectedHash() &&
        *ledgerData.getExpectedHash() !=
            appliedLedgerState->getLastClosedLedgerHeader().hash)
    {
#ifdef BUILD_TESTS
        if (ledgerCloseMeta)
        {
            ledgerCloseMeta->ledgerHeader() =
                appliedLedgerState->getLastClosedLedgerHeader();
            CLOG_ERROR(Ledger, "LedgerCloseMeta (base64): {}",
                       decoder::encode_b64(
                           xdr::xdr_to_opaque(ledgerCloseMeta->getXDR())));
        }
#endif
        throw std::runtime_error("Local node's ledger corrupted during close");
    }

#ifdef BUILD_TESTS
    if (ledgerCloseMeta)
    {
        ledgerCloseMeta->ledgerHeader() =
            appliedLedgerState->getLastClosedLedgerHeader();
        // Copy this before we move it into mNextMetaToEmit below
        mLastLedgerCloseMeta = *ledgerCloseMeta;
    }
#endif

    if (mMetaStream || mMetaDebugStream)
    {
        releaseAssert(ledgerCloseMeta);
        ledgerCloseMeta->ledgerHeader() =
            appliedLedgerState->getLastClosedLedgerHeader();

        // At this point we've got a complete meta and we can store it to the
        // member variable: if we throw while committing below, we will at worst
        // emit duplicate meta, when retrying.
        mNextMetaToEmit = std::move(ledgerCloseMeta);
        emitNextMeta();
    }

    // The next 8 steps happen in a relatively non-obvious, subtle order.
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
    //   occurs between commit and this step, core will attempt finalizing files
    //   again on restart.
    //
    // 4. Start background eviction scan for the next ledger, _after_ the commit
    //    so that it takes its snapshot of network setting from the
    //    committed state.
    //
    // 5. Copy any in-memory Soroban state for the snapshot invariant, if
    //    necessary. This occurs after commit has finished but before yielding
    //    back to main, so we can guarantee the in-memory state is immutable and
    //    up to date.
    //
    // 6. Start any queued checkpoint publishing, _after_ the commit so that
    //    it takes its snapshot of history-rows from the committed state, but
    //    _before_ we GC any buckets (because this is the step where the
    //    bucket refcounts are incremented for the duration of the publish).
    //
    // 7. GC unreferenced buckets. Only do this once publishes are in progress.
    //
    // 8. Finally, reflect newly closed ledger in LedgerManager's and Herder's
    //    states: maybe move into SYNCED state, trigger next ledger, etc. This
    //    also kicks off the invariant snapshot check, if necessary.

    // Step 1. Maybe queue the current checkpoint file for publishing; this
    // should not race with main, since publish on main begins strictly _after_
    // this call. There is a bug in the upgrade path where the initial
    // ledgerVers is used in some places during ledgerClose, and the upgraded
    // ledgerVers is used in other places (see comment in ledgerClosed).
    // On the ledger when an upgrade occurs, the ledger header will contain the
    // newly incremented ledgerVers. Because the history checkpoint must be
    // consistent with the ledger header, we must base checkpoints off the new
    // ledgerVers here and not the initial ledgerVers.
    auto& hm = mApp.getHistoryManager();
    hm.maybeQueueHistoryCheckpoint(ledgerSeq, maybeNewVersion);
    JITTER_INJECT_DELAY();

    // step 2
#ifdef BUILD_TESTS
    phaseStart = std::chrono::steady_clock::now();
#endif
    ltx.commit();
#ifdef BUILD_TESTS
    phaseEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.sqlCommitMs =
        std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
            .count();
    phaseStart = std::chrono::steady_clock::now();
#endif

#ifdef BUILD_TESTS
    mLatestTxResultSet = std::move(txResultSet);
#endif

    // step 3
    hm.maybeCheckpointComplete(ledgerSeq);
    JITTER_INJECT_DELAY();

    // Step 4
    if (protocolVersionStartsFrom(
            appliedLedgerState->getLastClosedLedgerHeader()
                .header.ledgerVersion,
            SOROBAN_PROTOCOL_VERSION))
    {
        // Construct an ApplyLedgerStateSnapshot from `appliedLedgerState`,
        // which holds the latest committed state, since the main thread has
        // not yet updated the non-apply lcl snapshot
        ApplyLedgerStateSnapshot snap(appliedLedgerState, mApp.getMetrics());
        mApp.getBucketManager().startBackgroundEvictionScan(
            std::move(snap), appliedLedgerState->getSorobanConfig());
    }

    // At this point, we've committed all changes to the Apply State for this
    // ledger. While the following functions will publish this state to other
    // subsystems, that's not relevant for Apply State phases since ApplyState
    // is only accessed by LedgerManager's apply threads.
    mApplyState.markEndOfCommitting();
    JITTER_INJECT_DELAY();

    // Step 5: kick off the snapshot invariant, if the timer has fired.
    // Both the apply-state snapshot and the in-memory Soroban state are
    // captured here at the same point (after commit), so they are guaranteed
    // to be from the same ledger.
    maybeRunSnapshotInvariantFromLedgerState(
        mApplyState.copyLedgerStateSnapshot());

    // Steps 6, 7, 8 are done in `advanceLedgerStateAndPublish`
    // NB: appliedLedgerState is invalidated after this call.
    if (threadIsMain())
    {
        advanceLedgerStateAndPublish(ledgerSeq, calledViaExternalize,
                                     ledgerData, std::move(appliedLedgerState),
                                     upgradeApplied);
    }
    else
    {
        auto cb = [this, ledgerSeq, calledViaExternalize, ledgerData,
                   appliedLedgerState = std::move(appliedLedgerState),
                   upgradeApplied]() mutable {
            advanceLedgerStateAndPublish(
                ledgerSeq, calledViaExternalize, ledgerData,
                std::move(appliedLedgerState), upgradeApplied);
        };
        mApp.postOnMainThread(std::move(cb), "advanceLedgerStateAndPublish");
    }
#ifdef BUILD_TESTS
    phaseEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.postCommitMs =
        std::chrono::duration<double, std::milli>(phaseEnd - phaseStart)
            .count();
#endif

    maybeSimulateSleep(mApp.getConfig(), txSet->sizeOpTotalForLogging(),
                       applyLedgerTime);
    std::chrono::duration<double> ledgerTimeSeconds = ledgerTime.Stop();
    CLOG_DEBUG(Perf, "Applied ledger {} in {} seconds", ledgerSeq,
               ledgerTimeSeconds.count());
    FrameMark;
}

void
LedgerManagerImpl::setLastClosedLedger(
    LedgerHeaderHistoryEntry const& lastClosed, bool rebuildInMemoryState)
{
    // NB: this method is a sort of half-apply that runs on main thread and
    // updates LCL without apply having happened any txs. It's only relevant
    // when finishing the _bucket-apply_ phase of catchup (which is not
    // transaction-apply, it's like "load any bucket state into the DB").
    ZoneScoped;
    releaseAssert(threadIsMain());
    mApplyState.assertSetupPhase();

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    header.current() = lastClosed.header;
    auto has = storePersistentStateAndLedgerHeaderInDB(
        header.current(), /* appendToCheckpoint */ false);
    ltx.commit();

    auto output = advanceApplySnapshotAndMakeLedgerState(
        lastClosed.header, has, /* sorobanConfig */ std::nullopt);
    advanceLastClosedLedgerState(output);

    auto ledgerVersion = lastClosed.header.ledgerVersion;
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_25))
    {
        PubKeyUtils::enableRustDalekVerify();
    }

    if (rebuildInMemoryState)
    {
        // This should not be additionally conditionalized on lv >= anything,
        // since we want to support SOROBAN_TEST_EXTRA_PROTOCOL > lv.
        mApplyState.compileAllContractsInLedger(ledgerVersion);
        mApplyState.populateInMemorySorobanState();
    }
    mApplyState.markEndOfSetupPhase();
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
    has.currentLedger = header.ledgerSeq;

    // No TXs to apply when manually advancing ledger header
    mApplyState.markStartOfApplying();
    mApplyState.markStartOfCommitting();
    mApplyState.manuallyAdvanceLedgerHeader(header);
    advanceLastClosedLedgerState(advanceApplySnapshotAndMakeLedgerState(
        header, has,
        /* sorobanConfig */ std::nullopt));
    mApplyState.markEndOfCommitting();
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

void
LedgerManagerImpl::advanceLastClosedLedgerState(
    CompleteConstLedgerStatePtr newLedgerState)
{
    releaseAssert(threadIsMain());
    releaseAssert(newLedgerState);

    SharedLockExclusive lock(mLedgerStateSnapshotMutex);
    if (mLastClosedLedgerState)
    {
        CLOG_DEBUG(
            Ledger, "Advancing LCL: {} -> {}",
            ledgerAbbrev(
                mLastClosedLedgerState->getLastClosedLedgerHeader().header),
            ledgerAbbrev(newLedgerState->getLastClosedLedgerHeader().header));
    }
    mLastClosedLedgerState = newLedgerState;
}

CompleteConstLedgerStatePtr
LedgerManagerImpl::buildLedgerState(
    LedgerHeader const& header, HistoryArchiveState const& has,
    CompleteConstLedgerStatePtr prevState,
    std::optional<SorobanNetworkConfig> sorobanConfig)
{
    mApplyState.threadInvariant();
    auto& bm = mApp.getBucketManager();

    // If the caller didn't provide a SorobanNetworkConfig, load it from the
    // BucketList (at this point the BucketList must have already been updated).
    if (!sorobanConfig && protocolVersionStartsFrom(header.ledgerVersion,
                                                    SOROBAN_PROTOCOL_VERSION))
    {
        auto liveData = std::make_shared<BucketListSnapshotData<LiveBucket>>(
            bm.getLiveBucketList());
        LedgerSnapshot ls(mApp.getMetrics(), std::move(liveData), header);
        sorobanConfig = SorobanNetworkConfig::loadFromLedger(ls);
    }

    LedgerHeaderHistoryEntry lcl;
    lcl.header = header;
    lcl.hash = xdrSha256(header);

    return std::make_shared<CompleteConstLedgerState>(
        bm.getLiveBucketList(), bm.getHotArchiveBucketList(), lcl, has,
        std::move(sorobanConfig), std::move(prevState),
        mNumHistoricalSnapshots);
}

CompleteConstLedgerStatePtr
LedgerManagerImpl::advanceApplySnapshotAndMakeLedgerState(
    LedgerHeader const& header, HistoryArchiveState const& has,
    std::optional<SorobanNetworkConfig> sorobanConfig)
{
    auto state = buildLedgerState(header, has, mApplyState.getLedgerState(),
                                  std::move(sorobanConfig));
    mApplyState.setLedgerState(state);
    return state;
}

LedgerStateSnapshot
LedgerManagerImpl::copyLedgerStateSnapshot() const
{
    // Apply thread must use the ApplyState's copyLedgerStateSnapshot.
    releaseAssert(!mApp.threadIsType(Application::ThreadType::APPLY));

    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    return LedgerStateSnapshot(mLastClosedLedgerState, mApp.getMetrics());
}

ApplyLedgerStateSnapshot
LedgerManagerImpl::copyApplyLedgerStateSnapshot() const
{
    // Apply-thread state may be ahead of mLastClosedLedgerState during
    // parallel ledger close, so always read from ApplyState.
    mApplyState.threadInvariant();
    return mApplyState.copyLedgerStateSnapshot();
}

void
LedgerManagerImpl::maybeUpdateLedgerStateSnapshot(
    LedgerStateSnapshot& snapshot) const
{
    SharedLockShared guard(mLedgerStateSnapshotMutex);
    releaseAssert(mLastClosedLedgerState);
    if (snapshot.getLedgerSeq() !=
        mLastClosedLedgerState->getLastClosedLedgerHeader().header.ledgerSeq)
    {
        snapshot =
            LedgerStateSnapshot(mLastClosedLedgerState, mApp.getMetrics());
    }
}

void
LedgerManagerImpl::ApplyState::setLedgerState(CompleteConstLedgerStatePtr state)
{
    assertWritablePhase();
    mLedgerState = std::move(state);
}

CompleteConstLedgerStatePtr
LedgerManagerImpl::ApplyState::getLedgerState() const
{
    releaseAssert(mLedgerState);
    return mLedgerState;
}

ApplyLedgerStateSnapshot
LedgerManagerImpl::ApplyState::copyLedgerStateSnapshot() const
{
    releaseAssert(mLedgerState);
    return ApplyLedgerStateSnapshot(mLedgerState, mAppConnector.getMetrics());
}

#ifdef BUILD_TESTS
void
LedgerManagerImpl::updateCanonicalStateForTesting(LedgerHeader const& header)
{
    releaseAssert(threadIsMain());

    HistoryArchiveState has;
    has.currentLedger = header.ledgerSeq;

    SharedLockExclusive lock(mLedgerStateSnapshotMutex);
    auto state =
        buildLedgerState(header, has, mLastClosedLedgerState, std::nullopt);

    mApplyState.setLedgerStateForTesting(state);

    mLastClosedLedgerState = state;
}
#endif
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
        // Cache protocol version to avoid repeated loadHeader() calls
        // in the per-TX loop below.
        auto const cachedLedgerVersion = header.ledgerVersion;
        bool const isV19OrLater = protocolVersionStartsFrom(
            cachedLedgerVersion, ProtocolVersion::V_19);
        std::map<AccountID, SequenceNumber> accToMaxSeq;

#ifdef BUILD_TESTS
        // If we have expected results, we assign them to the mutable tx results
        // here.
        std::optional<std::vector<TransactionResultPair>::const_iterator>
            expectedResultsIter = std::nullopt;
        auto expectedResults = ledgerData.getExpectedResults();
        if (expectedResults)
        {
            releaseAssert(expectedResults->results.size() ==
                          txSet.sizeTxTotal());
            expectedResultsIter =
                std::make_optional(expectedResults->results.begin());
        }
#endif

        bool mergeSeen = false;
        for (auto const& phase : txSet.getPhasesInApplyOrder())
        {
            for (auto const& tx : phase)
            {
                // Common per-tx fee processing logic, parameterized on the
                // active LTX (either a child for meta tracking, or the
                // parent directly when meta is disabled).
                auto processOneTxFee = [&](AbstractLedgerTxn& activeLtx) {
                    txResults.push_back(tx->processFeeSeqNum(
                        activeLtx, txSet.getTxBaseFee(tx)));
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

                    // Merge-op tracking (accToMaxSeq) is only needed for
                    // non-Soroban TXs. Soroban TXs have exactly one
                    // InvokeHostFunction op and can never contain
                    // ACCOUNT_MERGE, so mergeSeen will never be set.
                    // Use cached version to avoid per-TX loadHeader() calls.
                    if (isV19OrLater && !tx->isSoroban())
                    {
                        auto res = accToMaxSeq.emplace(tx->getSourceID(),
                                                       tx->getSeqNum());
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
                };

                if (ledgerCloseMeta)
                {
                    // Use a child LTX so we can capture per-tx changes
                    // for meta tracking via getChanges().
                    LedgerTxn ltxTx(ltx);
                    processOneTxFee(ltxTx);
                    ledgerCloseMeta->pushTxFeeProcessing(ltxTx.getChanges());
                    ltxTx.commit();
                }
                else
                {
                    // No meta needed — operate directly on parent LTX to
                    // avoid per-tx child LTX creation/destruction overhead.
                    processOneTxFee(ltx);
                }
                ++index;
            }
        }
        if (isV19OrLater && mergeSeen)
        {
            for (auto const& [accountID, seqNum] : accToMaxSeq)
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
    if (config.PREFETCH_BATCH_SIZE > 0 && !config.allBucketsInMemory())
    {
        UnorderedSet<LedgerKey> keys;
        for (auto const& phase : txSet.getPhases())
        {
            for (auto const& tx : phase)
            {
                tx->insertKeysForFeeProcessing(keys);
            }
        }
        ltx.prefetch(keys);
    }
}

void
LedgerManagerImpl::prefetchTransactionData(AbstractLedgerTxnParent& ltx,
                                           ApplicableTxSetFrame const& txSet,
                                           Config const& config)
{
    ZoneScoped;
    if (config.PREFETCH_BATCH_SIZE > 0 && !config.allBucketsInMemory())
    {
        UnorderedSet<LedgerKey> keysToPreFetch;
        for (auto const& phase : txSet.getPhases())
        {
            for (auto const& tx : phase)
            {
                tx->insertKeysForTxApply(keysToPreFetch);
            }
        }
        ltx.prefetch(keysToPreFetch);
    }
}

// C11: the C++ parallel-Soroban orchestration (applyThread,
// applySorobanStageClustersInParallel, checkAllTxBundleInvariants,
// applySorobanStage, applySorobanStages) lived here pre-refactor. All of
// it is replaced by LedgerManagerImpl::applySorobanPhaseRust below, which
// hands the whole phase to the Rust orchestrator. The corresponding
// declarations in LedgerManagerImpl.h are also removed.

// C11e: walk one (TxBundle, SorobanTxApplyResult) pair and fan the bridge
// outputs back into the per-TX result/meta plumbing:
//
//   - diagnostic events (always populated when diagnostics are enabled)
//     are pushed onto the per-op DiagnosticEventManager regardless of
//     success;
//   - on success, the contract events go onto the OpEventManager, the
//     operation result code is set to its op-specific SUCCESS, and for
//     InvokeHostFunction the SCVal return value is set on opMeta and the
//     InvokeHostFunctionResult.success Hash is filled with
//     SHA256(InvokeHostFunctionSuccessPreImage{returnValue, events});
//   - on failure, the operation result code is set to its op-specific
//     failure code (TRAPPED for InvokeHostFunction, MALFORMED for the
//     two TTL ops, mirroring the legacy C++ apply behaviour for now)
//     and the tx-level result is moved to txFAILED.
//
// On success, the refundable-fee tracker is advanced with the actual rent
// fee and contract-event byte size returned by Rust; finalizeFeeRefund
// later subtracts the unconsumed budget from feeCharged. On failure the
// tracker is reset by setInnermostError automatically (the source account
// gets the full refund).
static void
processSorobanPerTxResult(
    TxBundle const& bundle, SorobanTxApplyResult const& result,
    LedgerManagerImpl::PerTxDecodedRestores& decodedRestores,
    uint32_t ledgerVersion, uint32_t ledgerSeq,
    SorobanNetworkConfig const& sorobanConfig, Config const& appConfig,
    SorobanMetrics& sorobanMetrics, bool enableTxMeta)
{
    auto& resPayload = bundle.getResPayload();
    auto& opMeta =
        bundle.getEffects().getMeta().getOperationMetaBuilderAt(0);
    auto& diagnosticEvents = opMeta.getDiagnosticEventManager();

    for (auto const& deBuf : result.diagnostic_events)
    {
        DiagnosticEvent de;
        xdr::xdr_from_opaque(deBuf.data, de);
        diagnosticEvents.pushEvent(std::move(de));
    }

    if (!resPayload.isSuccess())
    {
        // Already failed upstream (validation / fee charge). The bridge
        // diagnostics are still useful but the OperationResult is gone.
        return;
    }

#ifdef BUILD_TESTS
    // BUILD_TESTS-only "txINTERNAL_ERROR" memo hook. Mirrors the legacy
    // applyOperations' maybeTriggerTestInternalError: a TX with that
    // memo text is meant to fail with txINTERNAL_ERROR.
    {
        auto const& env = bundle.getTx()->getEnvelope();
        Memo const* memo = nullptr;
        switch (env.type())
        {
        case ENVELOPE_TYPE_TX_V0:
            memo = &env.v0().tx.memo;
            break;
        case ENVELOPE_TYPE_TX:
            memo = &env.v1().tx.memo;
            break;
        case ENVELOPE_TYPE_TX_FEE_BUMP:
            memo = &env.feeBump().tx.innerTx.v1().tx.memo;
            break;
        default:
            break;
        }
        if (memo && memo->type() == MEMO_TEXT &&
            memo->text() == "txINTERNAL_ERROR")
        {
            resPayload.setInnermostError(txINTERNAL_ERROR);
            return;
        }
    }
#endif

    auto& opResult = resPayload.getOpResultAt(0);
    auto opType = opResult.tr().type();

    // Failure meter is marked here for early-exit failure paths (host
    // returned failure). Success meter is deferred until AFTER the
    // refundable-fee budget check below, since a TX whose host
    // succeeded but blew its refundable budget surfaces as
    // INSUFFICIENT_REFUNDABLE_FEE / txFAILED — the legacy
    // accumulateMetrics path likewise only marked success after that
    // check passed.
    if (opType == INVOKE_HOST_FUNCTION && !result.success)
    {
        sorobanMetrics.mHostFnOpFailure.Mark();
    }

    if (!result.success)
    {
        if (result.is_insufficient_refundable_fee)
        {
            switch (opType)
            {
            case INVOKE_HOST_FUNCTION:
                opResult.tr().invokeHostFunctionResult().code(
                    INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
                break;
            case EXTEND_FOOTPRINT_TTL:
                opResult.tr().extendFootprintTTLResult().code(
                    EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
                break;
            case RESTORE_FOOTPRINT:
                opResult.tr().restoreFootprintResult().code(
                    RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
                break;
            default:
                releaseAssert(false);
            }
        }
        else if (result.is_resource_limit_exceeded)
        {
            switch (opType)
            {
            case INVOKE_HOST_FUNCTION:
                opResult.tr().invokeHostFunctionResult().code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                break;
            case EXTEND_FOOTPRINT_TTL:
                opResult.tr().extendFootprintTTLResult().code(
                    EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
                break;
            case RESTORE_FOOTPRINT:
                opResult.tr().restoreFootprintResult().code(
                    RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
                break;
            default:
                releaseAssert(false);
            }
        }
        else if (result.is_entry_archived)
        {
            // The Rust apply pre-host walk detected an expired persistent
            // Soroban entry in the footprint that wasn't auto-restored.
            // Only InvokeHostFunction has a dedicated ENTRY_ARCHIVED
            // result code — the other Soroban op types either don't
            // observe archival here (RestoreFootprint is the auto-restore
            // path itself) or use TRAPPED.
            switch (opType)
            {
            case INVOKE_HOST_FUNCTION:
                opResult.tr().invokeHostFunctionResult().code(
                    INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                break;
            case EXTEND_FOOTPRINT_TTL:
                opResult.tr().extendFootprintTTLResult().code(
                    EXTEND_FOOTPRINT_TTL_MALFORMED);
                break;
            case RESTORE_FOOTPRINT:
                opResult.tr().restoreFootprintResult().code(
                    RESTORE_FOOTPRINT_MALFORMED);
                break;
            default:
                releaseAssert(false);
            }
        }
        else
        {
            switch (opType)
            {
            case INVOKE_HOST_FUNCTION:
                opResult.tr().invokeHostFunctionResult().code(
                    INVOKE_HOST_FUNCTION_TRAPPED);
                break;
            case EXTEND_FOOTPRINT_TTL:
                opResult.tr().extendFootprintTTLResult().code(
                    EXTEND_FOOTPRINT_TTL_MALFORMED);
                break;
            case RESTORE_FOOTPRINT:
                opResult.tr().restoreFootprintResult().code(
                    RESTORE_FOOTPRINT_MALFORMED);
                break;
            default:
                releaseAssert(false);
            }
        }
        resPayload.setInnermostError(txFAILED);
        return;
    }

    // Refundable-fee budget check. The host computed rent_fee for
    // whatever it did and event-size accumulates from the contract
    // events + return-value bytes; if the tx's declared refundable fee
    // can't cover both the rent and the events fee, the tx must fail
    // with INSUFFICIENT_REFUNDABLE_FEE — exactly as the legacy
    // InvokeHostFunctionOpFrame::consumeRefundableResources path did.
    // Crucially this runs BEFORE we set any success codes so a failure
    // here cleanly takes the failure branch.
    auto& refundTracker = resPayload.getRefundableFeeTracker();
    bool refundBudgetOk = true;
    if (refundTracker)
    {
        // Use the precomputed events-portion of the resource fee Rust
        // returned alongside the rent_fee — saves a per-tx FFI call
        // back into Rust to recompute the same value.
        refundBudgetOk =
            refundTracker->consumeRefundableSorobanResourcesPrecomputed(
                result.contract_event_size_bytes, result.rent_fee_consumed,
                result.refundable_fee_increment, diagnosticEvents);
    }
    if (!refundBudgetOk)
    {
        if (opType == INVOKE_HOST_FUNCTION)
        {
            sorobanMetrics.mHostFnOpFailure.Mark();
        }
        switch (opType)
        {
        case INVOKE_HOST_FUNCTION:
            opResult.tr().invokeHostFunctionResult().code(
                INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
            break;
        case EXTEND_FOOTPRINT_TTL:
            opResult.tr().extendFootprintTTLResult().code(
                EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
            break;
        case RESTORE_FOOTPRINT:
            opResult.tr().restoreFootprintResult().code(
                RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
            break;
        default:
            releaseAssert(false);
        }
        resPayload.setInnermostError(txFAILED);
        return;
    }
    // Refundable-fee check passed AND host succeeded — mark success.
    if (opType == INVOKE_HOST_FUNCTION)
    {
        sorobanMetrics.mHostFnOpSuccess.Mark();
    }

    // Decode contract events only when meta is enabled — the per-tx
    // event Vec is then forwarded to opMeta.getEventManager().setEvents
    // below. With meta off (apply-load benchmark default) the events
    // never leave the bridge buffer and we skip the per-event XDR
    // decode + Vec build.
    bool const metaEnabled = enableTxMeta;
    xdr::xvector<ContractEvent> contractEvents;
    if (metaEnabled)
    {
        contractEvents.reserve(result.contract_events.size());
        for (auto const& ceBuf : result.contract_events)
        {
            ContractEvent ce;
            xdr::xdr_from_opaque(ceBuf.data, ce);
            contractEvents.emplace_back(std::move(ce));
        }
    }

    switch (opType)
    {
    case INVOKE_HOST_FUNCTION:
    {
        // Use the pre-computed preimage hash that Rust returned along
        // with the event/return-value bytes. This skips a per-tx
        // XDR decode + re-encode + SHA-256 round-trip on the C++
        // side. Only decode the typed return value when meta is
        // enabled (it's needed for opMeta.setSorobanReturnValue but
        // nothing else).
        if (metaEnabled)
        {
            SCVal returnValue;
            xdr::xdr_from_opaque(result.return_value_xdr.data, returnValue);
            opMeta.setSorobanReturnValue(returnValue);
        }
        opResult.tr().invokeHostFunctionResult().code(
            INVOKE_HOST_FUNCTION_SUCCESS);
        Hash preimageHash;
        if (result.success_preimage_hash.data.size() == preimageHash.size())
        {
            std::memcpy(preimageHash.data(),
                        result.success_preimage_hash.data.data(),
                        preimageHash.size());
        }
        opResult.tr().invokeHostFunctionResult().success() = preimageHash;
        break;
    }
    case EXTEND_FOOTPRINT_TTL:
        opResult.tr().extendFootprintTTLResult().code(
            EXTEND_FOOTPRINT_TTL_SUCCESS);
        break;
    case RESTORE_FOOTPRINT:
        opResult.tr().restoreFootprintResult().code(
            RESTORE_FOOTPRINT_SUCCESS);
        break;
    default:
        releaseAssert(false);
    }

    if (!contractEvents.empty())
    {
        opMeta.getEventManager().setEvents(std::move(contractEvents));
    }

    // Build LedgerEntryChanges from the per-TX deltas. Empty prev = no
    // prior entry (CREATED); empty new = entry deleted (STATE +
    // REMOVED); both populated = STATE + UPDATED. RESTORED
    // reclassification is performed inside setLedgerChangesPreBuilt
    // via processOpLedgerEntryChanges using the restore-source maps
    // also returned by Rust.
    LedgerEntryChanges changes;
    changes.reserve(result.tx_changes.size() * 2);
    for (auto const& delta : result.tx_changes)
    {
        LedgerKey key;
        xdr::xdr_from_opaque(delta.key_xdr.data, key);
        bool hasPrev = !delta.prev_value_xdr.data.empty();
        bool hasNew = !delta.new_value_xdr.data.empty();
        if (hasPrev)
        {
            LedgerEntry prev;
            xdr::xdr_from_opaque(delta.prev_value_xdr.data, prev);
            changes.emplace_back(LEDGER_ENTRY_STATE);
            changes.back().state() = std::move(prev);
            if (hasNew)
            {
                LedgerEntry next;
                xdr::xdr_from_opaque(delta.new_value_xdr.data, next);
                changes.emplace_back(LEDGER_ENTRY_UPDATED);
                changes.back().updated() = std::move(next);
            }
            else
            {
                changes.emplace_back(LEDGER_ENTRY_REMOVED);
                changes.back().removed() = key;
            }
        }
        else if (hasNew)
        {
            LedgerEntry next;
            xdr::xdr_from_opaque(delta.new_value_xdr.data, next);
            changes.emplace_back(LEDGER_ENTRY_CREATED);
            changes.back().created() = std::move(next);
        }
    }

    // Decode the restore-source maps that processOpLedgerEntryChanges
    // consults to upgrade CREATED/UPDATED → RESTORED. Hot-archive
    // restores carry the archived value (pre-bump-lastModifiedLedgerSeq);
    // live-bucket restores carry the unchanged live value.
    // Phase-level dedup: if an earlier TX in the phase already
    // hot-archive-restored this key, this TX's hot_archive_restores
    // entry for the same key is a stale auto-restore hint that
    // shouldn't drive meta classification. processOpLedgerEntryChanges
    // would otherwise treat the recreated CREATED meta as a
    // restored-then-modified pair (UPDATED + RESTORED) instead of
    // the plain CREATED the test expects. Same logic applies to
    // live restores.
    // hot_archive_restores / live_restores were decoded once inside
    // applySorobanPhaseRust (and dedup'd against the phase-level seen
    // sets). Move them out here — we'd otherwise re-decode the same
    // byte buffers a second time.
    auto& hotArchiveRestores = decodedRestores.hotArchive;
    auto& liveRestores = decodedRestores.live;

    // Synthesize CREATED + REMOVED meta for hot-archive-restored keys
    // that the host then deleted within the same TX. The host's
    // e2e_invoke output omits a deleted RW entry entirely (no
    // encoded_new_value, no ttl_change above the prior live_until),
    // so Rust's tx_changes contains nothing for the key. Without
    // explicit CREATED meta there is no anchor for
    // processOpLedgerEntryChanges to convert into RESTORED, and the
    // test assertion `keysToRestore.empty()` fails. The auto-restore
    // bookkeeping (markRestoredFromHotArchive, hot archive removal)
    // still applies because hot_archive_restores carries the entry.
    UnorderedSet<LedgerKey> keysWithChanges;
    for (auto const& change : changes)
    {
        switch (change.type())
        {
        case LEDGER_ENTRY_CREATED:
            keysWithChanges.insert(LedgerEntryKey(change.created()));
            break;
        case LEDGER_ENTRY_UPDATED:
            keysWithChanges.insert(LedgerEntryKey(change.updated()));
            break;
        case LEDGER_ENTRY_STATE:
            keysWithChanges.insert(LedgerEntryKey(change.state()));
            break;
        case LEDGER_ENTRY_REMOVED:
            keysWithChanges.insert(change.removed());
            break;
        case LEDGER_ENTRY_RESTORED:
            keysWithChanges.insert(LedgerEntryKey(change.restored()));
            break;
        }
    }
    for (auto const& [hotKey, hotEntry] : hotArchiveRestores)
    {
        if (keysWithChanges.count(hotKey))
        {
            continue;
        }
        // Synthesize the create+delete pair so processOpLedgerEntryChanges
        // can convert CREATED → RESTORED. Append CREATED then REMOVED
        // so the meta order matches the legacy "restore then delete"
        // sequence. The CREATED carries the archived value as the
        // host saw it; processOpLedgerEntryChanges turns it into a
        // RESTORED with current ledgerSeq when it matches the
        // hotArchiveRestores entry.
        changes.emplace_back(LEDGER_ENTRY_CREATED);
        changes.back().created() = hotEntry;
        changes.back().created().lastModifiedLedgerSeq = ledgerSeq;
        changes.emplace_back(LEDGER_ENTRY_REMOVED);
        changes.back().removed() = hotKey;
    }

    opMeta.setLedgerChangesPreBuilt(std::move(changes), hotArchiveRestores,
                                    liveRestores, ledgerSeq);
}

rust::Vec<SorobanTxApplyResult>
LedgerManagerImpl::applySorobanPhaseRust(
    AbstractLedgerTxn& ltx, TxSetPhaseFrame const& phase,
    SorobanNetworkConfig const& sorobanConfig,
    Hash const& sorobanBasePrngSeed,
    std::vector<int64_t> const& perTxMaxRefundableFee, bool enableTxMeta,
    std::vector<PerTxDecodedRestores>& outPerTxDecodedRestores)
{
    ZoneScoped;

    // 1. Serialize each TX envelope into a flat Vec<CxxBuf> in apply
    //    order, plus per-cluster TX counts and per-stage cluster
    //    counts so Rust can rebuild the structure. This replaces the
    //    one-big-TransactionPhase XDR encode/decode round-trip with
    //    per-envelope encodes that Rust then decodes in parallel
    //    across the cluster worker pool — a large fixed-cost
    //    sequential decode (~10ms for 6000-tx phases) becomes a
    //    parallel ~1ms one.
    releaseAssert(phase.isParallel());
    auto const& applyStages = phase.getParallelStages();
    rust::Vec<CxxBuf> sorobanEnvelopes;
    rust::Vec<uint32_t> sorobanClusterSizes;
    rust::Vec<uint32_t> sorobanStageClusterCounts;
    {
        size_t totalTxs = 0;
        size_t totalClusters = 0;
        for (auto const& stage : applyStages)
        {
            totalClusters += stage.size();
            for (auto const& cluster : stage)
            {
                totalTxs += cluster.size();
            }
        }
        sorobanEnvelopes.reserve(totalTxs);
        sorobanClusterSizes.reserve(totalClusters);
        sorobanStageClusterCounts.reserve(applyStages.size());
        // Gather envelope pointers in apply order so we can XDR-encode
        // them in parallel — each `toCxxBuf(env)` does an
        // `xdr::xdr_to_opaque` walk of the entire envelope tree, which
        // is ~5us per TX. Sequential at 6000 TXs that's ~30ms of pre-
        // bridge overhead unique to the Rust apply path. Parallelizing
        // across worker threads shrinks it proportionally.
        std::vector<TransactionEnvelope const*> envPtrs;
        envPtrs.reserve(totalTxs);
        for (auto const& stage : applyStages)
        {
            sorobanStageClusterCounts.push_back(
                static_cast<uint32_t>(stage.size()));
            for (auto const& cluster : stage)
            {
                sorobanClusterSizes.push_back(
                    static_cast<uint32_t>(cluster.size()));
                for (auto const& tx : cluster)
                {
                    envPtrs.push_back(&tx->getEnvelope());
                }
            }
        }
        std::vector<CxxBuf> encoded(envPtrs.size());
        constexpr size_t MIN_PARALLEL = 256;
        if (envPtrs.size() < MIN_PARALLEL)
        {
            for (size_t i = 0; i < envPtrs.size(); ++i)
            {
                encoded[i] = toCxxBuf(*envPtrs[i]);
            }
        }
        else
        {
            constexpr size_t MAX_WORKERS = 8;
            size_t workerCount = std::min<size_t>(
                MAX_WORKERS, std::thread::hardware_concurrency());
            workerCount = std::max<size_t>(1, workerCount);
            workerCount = std::min(workerCount, envPtrs.size());
            std::vector<std::thread> threads;
            threads.reserve(workerCount);
            size_t baseChunk = envPtrs.size() / workerCount;
            size_t remainder = envPtrs.size() % workerCount;
            size_t begin = 0;
            for (size_t w = 0; w < workerCount; ++w)
            {
                size_t chunk = baseChunk + (w < remainder ? 1u : 0u);
                size_t end = begin + chunk;
                threads.emplace_back([&envPtrs, &encoded, begin, end]() {
                    for (size_t i = begin; i < end; ++i)
                    {
                        encoded[i] = toCxxBuf(*envPtrs[i]);
                    }
                });
                begin = end;
            }
            for (auto& t : threads)
            {
                t.join();
            }
        }
        for (auto& buf : encoded)
        {
            sorobanEnvelopes.push_back(std::move(buf));
        }
    }

    // 2. Build classic_prefetch by walking each Soroban TX's footprint,
    //    deduping non-Soroban keys (accounts, etc. — anything that
    //    isInMemoryType returns false for), and loading each from ltx.
    //    The Rust orchestrator's layered_get falls back to this map for
    //    classic-state reads. Source accounts that the C++ pre-pass
    //    already loaded into ltx (for fee charging / seqnum bumps) are
    //    visible here.
    //
    //    archived_prefetch covers the hot-archive probes that
    //    RestoreFootprint TXs need. The hot-archive snapshot is taken
    //    from the apply state's frozen LCL snapshot — RestoreFootprint
    //    only ever resurrects entries from the archive that existed at
    //    LCL time, so a phase-time snapshot is appropriate.
    rust::Vec<LedgerEntryInput> classicPrefetch =
        buildClassicPrefetchForPhase(ltx, phase);
    auto lclSnapshot = mApplyState.copyLedgerStateSnapshot();
    rust::Vec<LedgerEntryInput> archivedPrefetch =
        buildArchivedPrefetchForPhase(lclSnapshot, phase);

    // 3. Build CxxLedgerInfo. Mirrors the buildLedgerInfo helper in
    //    InvokeHostFunctionOpFrame.cpp.
    auto const& header = ltx.loadHeader().current();
    auto const& networkID = mApp.getNetworkID();
    CxxLedgerInfo ledgerInfo{};
    ledgerInfo.base_reserve = header.baseReserve;
    ledgerInfo.protocol_version = header.ledgerVersion;
    ledgerInfo.sequence_number = header.ledgerSeq;
    ledgerInfo.timestamp = header.scpValue.closeTime;
    ledgerInfo.memory_limit = sorobanConfig.txMemoryLimit();
    ledgerInfo.min_persistent_entry_ttl =
        sorobanConfig.stateArchivalSettings().minPersistentTTL;
    ledgerInfo.min_temp_entry_ttl =
        sorobanConfig.stateArchivalSettings().minTemporaryTTL;
    ledgerInfo.max_entry_ttl = sorobanConfig.stateArchivalSettings().maxEntryTTL;
    ledgerInfo.max_contract_size_bytes = sorobanConfig.maxContractSizeBytes();
    ledgerInfo.max_contract_data_entry_size_bytes =
        sorobanConfig.maxContractDataEntrySizeBytes();
    ledgerInfo.cpu_cost_params = toCxxBuf(sorobanConfig.cpuCostParams());
    ledgerInfo.mem_cost_params = toCxxBuf(sorobanConfig.memCostParams());
    ledgerInfo.network_id.reserve(networkID.size());
    for (auto c : networkID)
    {
        ledgerInfo.network_id.emplace_back(static_cast<unsigned char>(c));
    }

    auto rentFeeConfig = sorobanConfig.rustBridgeRentFeeConfiguration();
    auto feeConfig =
        sorobanConfig.rustBridgeFeeConfiguration(header.ledgerVersion);

    // Per-tx envelope byte size, in apply order — Rust uses these to
    // pre-compute the events portion of each TX's resource fee on the
    // cluster worker, saving a per-tx bridge call back from C++ in
    // the post-pass `RefundableFeeTracker::consume…` path.
    rust::Vec<uint32_t> perTxEnvelopeSizeBytes;
    {
        size_t total = 0;
        for (auto const& stage : applyStages)
        {
            for (auto const& cluster : stage)
            {
                total += cluster.size();
            }
        }
        perTxEnvelopeSizeBytes.reserve(total);
        for (auto const& stage : applyStages)
        {
            for (auto const& cluster : stage)
            {
                for (auto const& tx : cluster)
                {
                    perTxEnvelopeSizeBytes.push_back(static_cast<uint32_t>(
                        tx->getResources(/*useByteLimitInClassic=*/false,
                                         header.ledgerVersion)
                            .getVal(Resource::Type::TX_BYTE_SIZE)));
                }
            }
        }
    }

    // Wrap the 32-byte base PRNG seed in a CxxBuf for the bridge.
    // The Rust side derives per-TX seeds via SHA256(base || tx_num_be).
    CxxBuf prngSeedBuf{};
    prngSeedBuf.data = std::make_unique<std::vector<uint8_t>>();
    prngSeedBuf.data->assign(sorobanBasePrngSeed.begin(),
                             sorobanBasePrngSeed.end());

    // 4. Get the Rust SorobanState handle and the module cache.
    auto& sorobanStateBox =
        mApplyState.getInMemorySorobanStateForUpdate().getRustStateForBridge();
    auto const& moduleCache = mApplyState.getModuleCache();

    // 5. Call the bridge. Rust does the rest: walks stages → clusters →
    //    TXs in parallel via std::thread::scope, dispatches per-TX
    //    drivers, mutates SorobanState in place, returns ledger_updates.
    rust::Vec<int64_t> rustPerTxMaxRefundableFee;
    rustPerTxMaxRefundableFee.reserve(perTxMaxRefundableFee.size());
    for (auto v : perTxMaxRefundableFee)
    {
        rustPerTxMaxRefundableFee.push_back(v);
    }
    auto result = rust_bridge::apply_soroban_phase(
        *sorobanStateBox, *moduleCache,
        Config::CURRENT_LEDGER_PROTOCOL_VERSION, sorobanEnvelopes,
        sorobanClusterSizes, sorobanStageClusterCounts, prngSeedBuf,
        classicPrefetch, archivedPrefetch, ledgerInfo, rentFeeConfig,
        rustPerTxMaxRefundableFee,
        mApp.getConfig().ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, enableTxMeta,
        feeConfig, perTxEnvelopeSizeBytes);

    // 6. Apply the returned writes back to ltx. Soroban writes are
    //    pre-classified into init/live/dead by Rust (which already
    //    knew create vs update from `state.contains_*_by_hash` at
    //    fold time), so the C++ post-pass routes them straight through
    //    createWithoutLoading / updateWithoutLoading /
    //    eraseWithoutLoading without the wasCreate map walk over
    //    tx_changes the unsplit ledger_updates shape used to
    //    require. Classic side-effects (Account / Trustline / etc.)
    //    keep going through the existing load-and-mutate flow.
    //
    // Init/live ship just the encoded entry — C++ derives the
    // LedgerKey from the typed entry on its side via
    // `InternalLedgerEntry::ledgerKey` so the `key_xdr` half of the
    // pair never crossed the bridge. Dead writes ship just the
    // encoded LedgerKey (value bytes have no meaning for a delete).
    //
    // Parallel-decode: decode 24k entry XDR in parallel before the
    // sequential ltx.createWithoutLoading / updateWithoutLoading
    // calls. ltx is single-writer so the inserts must stay
    // sequential; only the per-entry XDR decode is parallelized.
    // Worker count capped at 8 (matches the typical apply-cluster
    // count) to avoid spinning up more threads than there's parallel
    // work for.
    auto parallelDecodeEntryXdrs =
        [](rust::Vec<RustBuf> const& entry_xdrs) {
            std::vector<LedgerEntry> entries(entry_xdrs.size());
            constexpr size_t MIN_PARALLEL = 1024;
            constexpr size_t MAX_WORKERS = 8;
            if (entry_xdrs.size() < MIN_PARALLEL)
            {
                for (size_t i = 0; i < entry_xdrs.size(); ++i)
                {
                    xdr::xdr_from_opaque(entry_xdrs[i].data, entries[i]);
                }
                return entries;
            }
            size_t workerCount =
                std::min<size_t>(MAX_WORKERS,
                                 std::thread::hardware_concurrency());
            workerCount = std::max<size_t>(1, workerCount);
            workerCount = std::min(workerCount, entry_xdrs.size());
            if (workerCount <= 1)
            {
                for (size_t i = 0; i < entry_xdrs.size(); ++i)
                {
                    xdr::xdr_from_opaque(entry_xdrs[i].data, entries[i]);
                }
                return entries;
            }
            std::vector<std::thread> threads;
            threads.reserve(workerCount);
            size_t baseChunk = entry_xdrs.size() / workerCount;
            size_t remainder = entry_xdrs.size() % workerCount;
            size_t begin = 0;
            for (size_t w = 0; w < workerCount; ++w)
            {
                size_t chunk = baseChunk + (w < remainder ? 1u : 0u);
                size_t end = begin + chunk;
                threads.emplace_back([&entry_xdrs, &entries, begin, end]() {
                    for (size_t i = begin; i < end; ++i)
                    {
                        xdr::xdr_from_opaque(entry_xdrs[i].data, entries[i]);
                    }
                });
                begin = end;
            }
            for (auto& t : threads)
            {
                t.join();
            }
            return entries;
        };

    {
        auto initEntries =
            parallelDecodeEntryXdrs(result.soroban_init_entry_xdrs);
        for (auto& entry : initEntries)
        {
            ltx.createWithoutLoading(InternalLedgerEntry(std::move(entry)));
        }
    }
    {
        auto liveEntries =
            parallelDecodeEntryXdrs(result.soroban_live_entry_xdrs);
        for (auto& entry : liveEntries)
        {
            ltx.updateWithoutLoading(InternalLedgerEntry(std::move(entry)));
        }
    }
    for (auto const& key_xdr : result.soroban_dead_key_xdrs)
    {
        LedgerKey key;
        xdr::xdr_from_opaque(key_xdr.data, key);
        ltx.eraseWithoutLoading(key);
    }
    for (auto const& update : result.classic_updates)
    {
        LedgerKey key;
        xdr::xdr_from_opaque(update.key_xdr.data, key);
        if (update.value_xdr.data.empty())
        {
            if (ltx.load(key))
            {
                ltx.erase(key);
            }
            continue;
        }
        LedgerEntry entry;
        xdr::xdr_from_opaque(update.value_xdr.data, entry);
        if (auto existing = ltx.load(key))
        {
            existing.current() = std::move(entry);
        }
        else
        {
            ltx.create(InternalLedgerEntry(std::move(entry)));
        }
    }

    // Notify the LedgerTxn of all restored Soroban entries so the bucket
    // commit path tracks them correctly. The legacy parallel-apply
    // orchestration called ltx.markRestoredFromHotArchive / Live for
    // each (data, ttl) pair before commit; without it,
    // ltx.getRestoredHotArchiveKeys() / getRestoredLiveBucketListKeys()
    // return empty, the hot archive doesn't get the restored entry
    // removed via addHotArchiveBatch's restoredKeys parameter, and
    // tests like "multiple version of same key in a single eviction
    // scan" see a stale hot-archive entry alongside the live-BL one.
    //
    // Rust returns hot_archive_restores / live_restores as separate
    // LedgerEntryUpdates per data and TTL key (not paired). Decode each
    // byte buffer exactly once here and stash both keyed-by-LedgerKey
    // (for processSorobanPerTxResult's meta classification) and
    // grouped-by-TTL-hash (for ltx.markRestoredFrom* which wants the
    // data + TTL pair together: data entry's getTTLKey().key_hash
    // matches TTL entry's data.ttl().key_hash).
    //
    // Phase-level dedup of the markRestored side effect: if multiple
    // TXs in the same phase auto-restore the same key, only the first
    // call should mark it (markRestoredFrom* asserts uniqueness on its
    // internal map and would otherwise crash on duplicates).
    outPerTxDecodedRestores.assign(result.per_tx.size(),
                                   PerTxDecodedRestores{});
    UnorderedSet<Hash> alreadyHotRestored;
    UnorderedSet<Hash> alreadyLiveRestored;
    auto decodeAndMarkRestored =
        [&](rust::Vec<LedgerEntryUpdate> const& vec,
            UnorderedSet<LedgerKey>& alreadyLedgerKeyDedup,
            UnorderedSet<Hash>& alreadyTtlHashDedup,
            UnorderedMap<LedgerKey, LedgerEntry>& outPerTxByLedgerKey,
            bool fromHotArchive) {
            UnorderedMap<Hash, LedgerEntry> dataByTtlHash;
            UnorderedMap<Hash, LedgerEntry> ttlByHash;
            outPerTxByLedgerKey.reserve(vec.size());
            for (auto const& u : vec)
            {
                LedgerKey k;
                LedgerEntry e;
                xdr::xdr_from_opaque(u.key_xdr.data, k);
                xdr::xdr_from_opaque(u.value_xdr.data, e);
                // Per-meta dedup against the phase-level seen set.
                // processSorobanPerTxResult uses the resulting per-TX
                // map directly.
                if (alreadyLedgerKeyDedup.insert(k).second)
                {
                    if (e.data.type() == TTL)
                    {
                        ttlByHash.emplace(e.data.ttl().keyHash, e);
                    }
                    else if (e.data.type() == CONTRACT_DATA ||
                             e.data.type() == CONTRACT_CODE)
                    {
                        auto ttlKey = getTTLKey(e);
                        dataByTtlHash.emplace(ttlKey.ttl().keyHash, e);
                    }
                    outPerTxByLedgerKey.emplace(std::move(k), std::move(e));
                }
            }
            for (auto& [hash, dataEntry] : dataByTtlHash)
            {
                auto ttlIt = ttlByHash.find(hash);
                if (ttlIt == ttlByHash.end())
                {
                    continue;
                }
                if (!alreadyTtlHashDedup.insert(hash).second)
                {
                    continue;
                }
                if (fromHotArchive)
                {
                    ltx.markRestoredFromHotArchive(dataEntry, ttlIt->second);
                }
                else
                {
                    ltx.markRestoredFromLiveBucketList(dataEntry,
                                                      ttlIt->second);
                }
            }
        };
    UnorderedSet<LedgerKey> alreadyHotRestoredKeys;
    UnorderedSet<LedgerKey> alreadyLiveRestoredKeys;
    for (size_t i = 0; i < result.per_tx.size(); ++i)
    {
        auto const& tx = result.per_tx[i];
        auto& perTxOut = outPerTxDecodedRestores[i];
        decodeAndMarkRestored(tx.hot_archive_restores,
                              alreadyHotRestoredKeys, alreadyHotRestored,
                              perTxOut.hotArchive, /*fromHotArchive=*/true);
        decodeAndMarkRestored(tx.live_restores, alreadyLiveRestoredKeys,
                              alreadyLiveRestored, perTxOut.live,
                              /*fromHotArchive=*/false);
    }

    // Return per_tx so the caller can walk applyStages in lockstep and
    // populate per-TX OperationResult codes / meta. Done in
    // applyParallelPhase via processSorobanPerTxResult.
    return std::move(result.per_tx);
}

void
LedgerManagerImpl::processResultAndMeta(
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    uint32_t txIndex, TransactionMetaBuilder& txMetaBuilder,
    TransactionFrameBase const& tx, MutableTransactionResultBase const& result,
    TransactionResultSet& txResultSet)
{
    ZoneScoped;
    TransactionResultPair resultPair;
    resultPair.transactionHash = tx.getContentsHash();
    resultPair.result = result.getXDR();

    if (result.isSuccess())
    {
        if (tx.isSoroban())
        {
            mApplyState.getMetrics().mSorobanTransactionApplySucceeded.inc();
        }
        mApplyState.getMetrics().mTransactionApplySucceeded.inc();
    }
    else
    {
        if (tx.isSoroban())
        {
            mApplyState.getMetrics().mSorobanTransactionApplyFailed.inc();
        }
        mApplyState.getMetrics().mTransactionApplyFailed.inc();
    }

    // First gather the TransactionResultPair into the TxResultSet
    // for hashing into the ledger header.
    txResultSet.results.emplace_back(resultPair);

    if (ledgerCloseMeta)
    {
        auto metaXDR = txMetaBuilder.finalize(result.isSuccess());
#ifdef BUILD_TESTS
        if (!mApp.getConfig().DISABLE_TX_META_FOR_TESTING)
        {
            mLastLedgerTxMeta.emplace_back(metaXDR);
        }
#endif

        ledgerCloseMeta->setTxProcessingMetaAndResultPair(
            std::move(metaXDR), std::move(resultPair), txIndex);
    }
    else
    {
#ifdef BUILD_TESTS
        if (!mApp.getConfig().DISABLE_TX_META_FOR_TESTING)
        {
            mLastLedgerTxMeta.emplace_back(
                txMetaBuilder.finalize(result.isSuccess()));
        }
#endif
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
#ifdef BUILD_TESTS
    auto txSubStart = std::chrono::steady_clock::now();
#endif
    size_t numTxs = txSet.sizeTxTotal();
    size_t numOps = txSet.sizeOpTotal();
    releaseAssert(numTxs == mutableTxResults.size());
    uint32_t index = 0;

    // Record counts
    if (numTxs > 0)
    {
        mApplyState.getMetrics().mTransactionCount.Update(
            static_cast<int64_t>(numTxs));
        TracyPlot("ledger.transaction.count", static_cast<int64_t>(numTxs));

        mApplyState.getMetrics().mOperationCount.Update(
            static_cast<int64_t>(numOps));
        TracyPlot("ledger.operation.count", static_cast<int64_t>(numOps));
        CLOG_INFO(Tx, "applying ledger {} ({})",
                  ltx.loadHeader().current().ledgerSeq, txSet.summary());
    }
    TransactionResultSet txResultSet;
    txResultSet.results.reserve(numTxs);

#ifdef BUILD_TESTS
    auto txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.applyTxSetupMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();
    txSubStart = std::chrono::steady_clock::now();
#endif
    prefetchTransactionData(mApp.getLedgerTxnRoot(), txSet, mApp.getConfig());
#ifdef BUILD_TESTS
    txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.prefetchTxDataMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();
    txSubStart = std::chrono::steady_clock::now();
#endif
    auto phases = txSet.getPhasesInApplyOrder();

    Hash sorobanBasePrngSeed = txSet.getContentsHash();

    // There is no need to populate the transaction meta if we are not going
    // to output it. This flag will make most of the meta operations to be
    // no-op (this is a bit more readable alternative to handling the
    // optional value across all the codebase).
    bool enableTxMeta = ledgerCloseMeta != nullptr;
#ifdef BUILD_TESTS
    // In tests we want to always enable tx meta because we store it in
    // mLastLedgerTxMeta, unless explicitly disabled for benchmarking.
    if (!mApp.getConfig().DISABLE_TX_META_FOR_TESTING)
    {
        enableTxMeta = true;
    }
#endif
#ifdef BUILD_TESTS
    txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.applyTxMidSetupMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();
#endif
#ifdef BUILD_TESTS
    txSubStart = std::chrono::steady_clock::now();
#endif
    std::optional<SorobanNetworkConfig> sorobanConfig;
    if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        sorobanConfig =
            std::make_optional(SorobanNetworkConfig::loadFromLedger(ltx));
    }
#ifdef BUILD_TESTS
    txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.loadSorobanConfigMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();
    mLastPhaseTimings.applySeqClassicMs = 0;
#endif
    std::vector<ApplyStage> applyStages;
    for (auto const& phase : phases)
    {
        if (phase.isParallel())
        {
            try
            {
                releaseAssert(sorobanConfig.has_value());
#ifdef BUILD_TESTS
                auto parPhaseStart = std::chrono::steady_clock::now();
#endif
                applyParallelPhase(phase, applyStages, mutableTxResults, index,
                                   ltx, enableTxMeta, *sorobanConfig,
                                   sorobanBasePrngSeed);
#ifdef BUILD_TESTS
                auto parPhaseEnd = std::chrono::steady_clock::now();
                mLastPhaseTimings.applyParallelPhaseTotalMs =
                    std::chrono::duration<double, std::milli>(parPhaseEnd -
                                                              parPhaseStart)
                        .count();
#endif
            }
            catch (std::exception const& e)
            {
                printErrorAndAbort("Exception during applyParallelPhase: ",
                                   e.what());
            }
            catch (...)
            {
                printErrorAndAbort(
                    "Unknown exception during applyParallelPhase");
            }
        }
        else
        {
#ifdef BUILD_TESTS
            txSubStart = std::chrono::steady_clock::now();
#endif
            applySequentialPhase(phase, mutableTxResults, index, ltx,
                                 enableTxMeta, sorobanConfig,
                                 sorobanBasePrngSeed, ledgerCloseMeta,
                                 txResultSet);
#ifdef BUILD_TESTS
            txSubEnd = std::chrono::steady_clock::now();
            mLastPhaseTimings.applySeqClassicMs +=
                std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
                    .count();
#endif
        }
    }

#ifdef BUILD_TESTS
    txSubStart = std::chrono::steady_clock::now();
#endif
    processPostTxSetApply(phases, applyStages, ltx, ledgerCloseMeta,
                          txResultSet);
#ifdef BUILD_TESTS
    txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.postTxSetApplyMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();
    txSubStart = std::chrono::steady_clock::now();
#endif

    // Update cluster and stage metrics
    if (!applyStages.empty())
    {
        size_t maxClusters = 0;
        for (auto const& stage : applyStages)
        {
            maxClusters = std::max(maxClusters, stage.numClusters());
        }
        mApplyState.getMetrics().mMaxClustersPerLedger.set_count(maxClusters);
        mApplyState.getMetrics().mStagesPerLedger.set_count(applyStages.size());
    }

    logTxApplyMetrics(ltx, numTxs, numOps);
#ifdef BUILD_TESTS
    txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.applyTxTailMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();

    txSubStart = std::chrono::steady_clock::now();
#endif
    applyStages.clear();
#ifdef BUILD_TESTS
    txSubEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.destroyApplyStagesMs =
        std::chrono::duration<double, std::milli>(txSubEnd - txSubStart)
            .count();
#endif
    return txResultSet;
}

void
LedgerManagerImpl::applyParallelPhase(
    TxSetPhaseFrame const& phase, std::vector<stellar::ApplyStage>& applyStages,
    std::vector<stellar::MutableTxResultPtr> const& mutableTxResults,
    uint32_t& index, stellar::AbstractLedgerTxn& ltx, bool enableTxMeta,
    SorobanNetworkConfig const& sorobanConfig, Hash const& sorobanBasePrngSeed)
{
    ZoneScoped;

    auto const& txSetStages = phase.getParallelStages();

    applyStages.reserve(txSetStages.size());

#ifdef BUILD_TESTS
    auto bundleStart = std::chrono::steady_clock::now();
#endif
    for (auto const& stage : txSetStages)
    {
        std::vector<Cluster> applyClusters;
        applyClusters.reserve(stage.size());

        for (auto const& cluster : stage)
        {
            Cluster applyCluster;
            applyCluster.reserve(cluster.size());

            for (auto const& tx : cluster)
            {
                auto& mutableTxResult = mutableTxResults.at(index);
                applyCluster.emplace_back(
                    mApp.getAppConnector(), tx, *mutableTxResult,
                    ltx.loadHeader().current().ledgerVersion, index,
                    enableTxMeta);

                // The refundable-fee tracker + non-refundable fee
                // accounting are now performed inside
                // commonPreApplyForSoroban (below, in the per-TX
                // pre-apply loop), which delegates to the legacy
                // commonPreApply that already does this work. No
                // separate explicit call here.

                // Use txBundle.getTxNum() to get this transactions
                // index from now on
                ++index;

                // Emit fee event before applying the transaction. This
                // technically has to be emitted during
                // processFeesSeqNums, but since tx meta doesn't exist
                // at that point, we emit the event as soon as possible.
                applyCluster.back()
                    .getEffects()
                    .getMeta()
                    .getTxEventManager()
                    .newFeeEvent(tx->getFeeSourceID(),
                                 mutableTxResult->getFeeCharged(),
                                 TransactionEventStage::
                                     TRANSACTION_EVENT_STAGE_BEFORE_ALL_TXS);
            }
            applyClusters.emplace_back(std::move(applyCluster));
        }
        applyStages.emplace_back(std::move(applyClusters));
    }
#ifdef BUILD_TESTS
    auto bundleEnd = std::chrono::steady_clock::now();
    mLastPhaseTimings.buildTxBundlesMs =
        std::chrono::duration<double, std::milli>(bundleEnd - bundleStart)
            .count();
#endif

    // C11i: bump source-account seqNum for each Soroban TX before the
    // Rust apply runs, plus run signature checking and per-tx
    // commonValid. Splits into a parallel read-only pass that builds the
    // SignatureChecker and runs commonValid + signature verification
    // against an immutable LCL snapshot, followed by a sequential write
    // pass that bumps seqNum, removes one-time signers, and pushes
    // txChangesBefore on the live ltx.
    //
    // Source-account existence is consulted out of the live ltx
    // (post-classic-phase) so a same-ledger account-merge surfaces
    // txNO_ACCOUNT here, mirroring the legacy commonValid behaviour.
    // Bundles whose source has been destroyed are excluded from both
    // the parallel and write batches.
    std::vector<TxBundle const*> readyBundles;
    {
        size_t totalReady = 0;
        for (auto const& stage : applyStages)
        {
            for (auto const& bundle : stage)
            {
                (void)bundle;
                ++totalReady;
            }
        }
        readyBundles.reserve(totalReady);
        for (auto const& stage : applyStages)
        {
            for (auto const& bundle : stage)
            {
                auto const& tx = bundle.getTx();
                bool sourceExists;
                {
                    LedgerTxn srcLtx(ltx);
                    auto src =
                        stellar::loadAccount(srcLtx, tx->getSourceID());
                    sourceExists = static_cast<bool>(src);
                }
                if (!sourceExists)
                {
                    bundle.getResPayload().setInnermostError(txNO_ACCOUNT);
                    continue;
                }
                readyBundles.emplace_back(&bundle);
            }
        }
    }

    // Per-bundle ParallelPreApplyInfo, populated by the read-only pass
    // and consumed by the write pass. Indexed identically to
    // readyBundles.
    std::vector<ParallelPreApplyInfo> preApplyInfos(readyBundles.size());

    if (!readyBundles.empty())
    {
        ZoneNamedN(zone, "preParallelApplyReadOnly", true);
        // The read-only pass must see same-ledger classic-phase
        // mutations to source-account signers / balances (e.g. a
        // classic SetOptions(masterWeight=0) ahead of a Soroban TX must
        // surface txBAD_AUTH on the Soroban TX). The LCL bucket
        // snapshot alone doesn't reflect those changes, and the live
        // ltx is single-thread-affine so it can't back parallel reads.
        // Pre-load every account that a Soroban TX could consult
        // during signature verification (tx source, fee source, op
        // sources) from the live ltx into an immutable overlay map,
        // then hand each worker an overlay snapshot that consults the
        // overlay first and falls back to the LCL bucket snapshot.
        auto overlay = std::make_shared<
            UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>>();
        {
            UnorderedSet<LedgerKey> keys;
            keys.reserve(readyBundles.size() * 2);
            for (auto const* bundle : readyBundles)
            {
                auto const& tx = bundle->getTx();
                keys.insert(accountKey(tx->getSourceID()));
                keys.insert(accountKey(tx->getFeeSourceID()));
                for (auto const& op : tx->getOperationFrames())
                {
                    keys.insert(accountKey(op->getSourceID()));
                }
            }
            overlay->reserve(keys.size());
            for (auto const& key : keys)
            {
                auto entry = ltx.loadWithoutRecord(key);
                if (entry)
                {
                    overlay->emplace(
                        key, std::make_shared<LedgerEntry const>(
                                 entry.current()));
                }
                else
                {
                    overlay->emplace(key, nullptr);
                }
            }
        }
        auto applySnapshot = mApplyState.copyLedgerStateSnapshot();
        size_t workerCount = 1;
        if (auto hardwareConcurrency = std::thread::hardware_concurrency();
            hardwareConcurrency > 1)
        {
            workerCount = hardwareConcurrency;
        }
        workerCount = std::min(workerCount, readyBundles.size());

        auto runRange = [&](size_t begin, size_t end) {
            // Each worker constructs its own LedgerSnapshot over the
            // shared ApplyLedgerStateSnapshot + the shared overlay.
            // The snapshot itself is immutable across the apply phase
            // and the overlay is built once before any worker starts,
            // so concurrent reads are safe.
            LedgerSnapshot ls(std::make_unique<OverlayApplySnapshot>(
                applySnapshot, overlay));
            auto& app = mApp.getAppConnector();
            for (size_t i = begin; i < end; ++i)
            {
                auto const* bundle = readyBundles[i];
                bundle->getTx()->preParallelApplyForSorobanReadOnly(
                    app, ls, bundle->getEffects().getMeta(),
                    bundle->getResPayload(), sorobanConfig,
                    preApplyInfos[i]);
            }
        };

        if (workerCount <= 1)
        {
            runRange(0, readyBundles.size());
        }
        else
        {
            std::vector<std::thread> threads;
            threads.reserve(workerCount);
            size_t begin = 0;
            auto const baseChunkSize = readyBundles.size() / workerCount;
            auto const remainder = readyBundles.size() % workerCount;
            for (size_t w = 0; w < workerCount; ++w)
            {
                auto const chunkSize =
                    baseChunkSize + (w < remainder ? 1u : 0u);
                auto const end = begin + chunkSize;
                threads.emplace_back(runRange, begin, end);
                begin = end;
            }
            for (auto& t : threads)
            {
                t.join();
            }
        }
    }

    // Sequential write pass: applies the recorded seqnum bumps, signer
    // removals, and meta pushes against the shared ltx.
    {
        ZoneNamedN(zone, "preParallelApplyWrite", true);
        for (size_t i = 0; i < readyBundles.size(); ++i)
        {
            auto const* bundle = readyBundles[i];
            bundle->getTx()->preParallelApplyForSorobanWrite(
                mApp.getAppConnector(), ltx,
                bundle->getEffects().getMeta(), preApplyInfos[i]);
        }
    }

    // C10c: switched from the old C++ orchestration (applySorobanStages
    // + GlobalParallelApplyLedgerState + per-TX C++ apply via the cxx
    // invoke_host_function bridge) to the new single-call Rust apply
    // phase. The applyStages vec built above is no longer consumed by
    // the apply path; it stays for the per-TX setup events that the
    // outer loop already emitted (fee event etc.).
    //
    // Per-TX results processing (meta, fee refund) still needs to be
    // threaded through — see TODOs inside applySorobanPhaseRust.
    // Build the per-TX max_refundable_fee vector in apply order so the
    // Rust orchestrator can drop a TX's writes when the host-reported
    // rent_fee exceeds the TX's budget — the equivalent of the legacy
    // doApply's "if !consume return false" + LedgerTxn rollback.
    std::vector<int64_t> perTxMaxRefundableFee;
    {
        size_t totalTxs = 0;
        for (auto const& stage : applyStages)
        {
            for (auto const& bundle : stage)
            {
                (void)bundle;
                ++totalTxs;
            }
        }
        perTxMaxRefundableFee.reserve(totalTxs);
        for (auto const& stage : applyStages)
        {
            for (auto const& bundle : stage)
            {
                auto const& tracker =
                    bundle.getResPayload().getRefundableFeeTracker();
                if (tracker)
                {
                    perTxMaxRefundableFee.push_back(
                        tracker->getMaximumRefundableFee());
                }
                else
                {
                    perTxMaxRefundableFee.push_back(
                        std::numeric_limits<int64_t>::max());
                }
            }
        }
    }
    // perTxDecodedRestores is populated by applySorobanPhaseRust in
    // lockstep with the returned perTxResults: each element holds the
    // already-decoded LedgerKey → LedgerEntry maps for that TX's
    // hot-archive / live-bucket restores. processSorobanPerTxResult
    // consumes them directly instead of re-decoding the same byte
    // buffers a second time. The phase-level dedup against duplicates
    // also happens inside applySorobanPhaseRust, so the maps here are
    // already filtered.
    std::vector<PerTxDecodedRestores> perTxDecodedRestores;
    auto perTxResults = applySorobanPhaseRust(
        ltx, phase, sorobanConfig, sorobanBasePrngSeed, perTxMaxRefundableFee,
        enableTxMeta, perTxDecodedRestores);
    releaseAssert(perTxResults.size() == perTxDecodedRestores.size());

    // C11e: walk applyStages and perTxResults in lockstep. The bridge
    // returns per_tx in stage-order ⨯ cluster-order ⨯ tx-order, which is
    // the same order ApplyStage::Iterator produces. Each call mutates
    // OperationResult / meta on the matching TxBundle.
    auto const& header = ltx.loadHeader().current();
    auto const ledgerVersion = header.ledgerVersion;
    auto const ledgerSeq = header.ledgerSeq;
    auto const& appConfig = mApp.getConfig();
    size_t txIdx = 0;
    for (auto const& stage : applyStages)
    {
        for (auto const& bundle : stage)
        {
            releaseAssert(txIdx < perTxResults.size());
            processSorobanPerTxResult(
                bundle, perTxResults[txIdx], perTxDecodedRestores[txIdx],
                ledgerVersion, ledgerSeq, sorobanConfig, appConfig,
                mApplyState.getMetrics().mSorobanMetrics, enableTxMeta);
            ++txIdx;
        }
    }
    releaseAssert(txIdx == perTxResults.size());

    // meta will be processed in processPostTxSetApply
}

void
LedgerManagerImpl::applySequentialPhase(
    TxSetPhaseFrame const& phase,
    std::vector<MutableTxResultPtr> const& mutableTxResults, uint32_t& index,
    AbstractLedgerTxn& ltx, bool enableTxMeta,
    std::optional<SorobanNetworkConfig const> const& sorobanConfig,
    Hash const& sorobanBasePrngSeed,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    TransactionResultSet& txResultSet)
{
    for (auto const& tx : phase)
    {
        ZoneNamedN(txZone, "applyTransaction", true);
        auto& mutableTxResult = *mutableTxResults.at(index);

        auto txTime = mApplyState.getMetrics().mTransactionApply.TimeScope();
        TransactionMetaBuilder tm(enableTxMeta, *tx,
                                  ltx.loadHeader().current().ledgerVersion,
                                  mApp.getAppConnector());
        // Emit fee event before applying the transaction. This
        // technically has to be emitted during processFeesSeqNums, but
        // since tx meta doesn't exist at that point, we emit the event
        // as soon as possible.
        tm.getTxEventManager().newFeeEvent(
            tx->getFeeSourceID(), mutableTxResult.getFeeCharged(),
            TransactionEventStage::TRANSACTION_EVENT_STAGE_BEFORE_ALL_TXS);
        CLOG_DEBUG(Tx, " tx#{} = {} ops={} txseq={} (@ {})", index,
                   hexAbbrev(tx->getContentsHash()), tx->getNumOperations(),
                   tx->getSeqNum(),
                   mApp.getConfig().toShortString(tx->getSourceID()));

        Hash subSeed = sorobanBasePrngSeed;
        // If tx can use the seed, we need to compute a sub-seed for it.
        if (tx->isSoroban())
        {
            // The index used here was originally a uint64_t, but it was removed
            // because it was duplicated info. We need to still use a uint64_t
            // because the type of the integer affects the prng seed, which
            // is observable in the protocol.
            subSeed =
                subSha256(sorobanBasePrngSeed, static_cast<uint64_t>(index));
        }

        tx->apply(mApp.getAppConnector(), ltx, tm, mutableTxResult,
                  sorobanConfig, subSeed);
        tx->processPostApply(mApp.getAppConnector(), ltx, tm, mutableTxResult);

        tm.maybeSetRefundableFeeMeta(mutableTxResult.getRefundableFeeTracker());

        // We process meta early here instead of in
        // processPostTxSetApply because the non-parallel path does not
        // used TransactionFrame::processPostTxSetApply at the moment.
        processResultAndMeta(ledgerCloseMeta, index, tm, *tx, mutableTxResult,
                             txResultSet);

        ++index;
    }
}

void
LedgerManagerImpl::processPostTxSetApply(
    std::vector<TxSetPhaseFrame> const& phases,
    std::vector<ApplyStage> const& applyStages, AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    TransactionResultSet& txResultSet)
{
    ZoneScoped;
    for (auto const& phase : phases)
    {
        if (phase.isParallel())
        {
            for (auto const& stage : applyStages)
            {
                for (auto const& txBundle : stage)
                {
                    if (ledgerCloseMeta)
                    {
                        // Use child LTX for meta change tracking.
                        LedgerTxn ltxInner(ltx);
                        txBundle.getTx()->processPostTxSetApply(
                            mApp.getAppConnector(), ltxInner,
                            txBundle.getResPayload(),
                            txBundle.getEffects()
                                .getMeta()
                                .getTxEventManager());

                        ledgerCloseMeta->setPostTxApplyFeeProcessing(
                            ltxInner.getChanges(), txBundle.getTxNum());
                        ltxInner.commit();
                    }
                    else
                    {
                        // No meta — operate directly on parent LTX.
                        txBundle.getTx()->processPostTxSetApply(
                            mApp.getAppConnector(), ltx,
                            txBundle.getResPayload(),
                            txBundle.getEffects()
                                .getMeta()
                                .getTxEventManager());
                    }

                    // setPostTxApplyFeeProcessing can update the feeCharged in
                    // the result, so this needs to be done after
                    processResultAndMeta(ledgerCloseMeta, txBundle.getTxNum(),
                                         txBundle.getEffects().getMeta(),
                                         *txBundle.getTx(),
                                         txBundle.getResPayload(), txResultSet);
                }
            }
        }
        // setPostTxApplyFeeProcessing is not used in the non-parallel
        // path, so we don't need to call it here, but we will need to add
        // support for it if we add any post tx set apply processing in the
        // non-parallel phase.
    }
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
    mApplyState.getMetrics().mPrefetchHitRate.Update(std::llround(hitRate));
    TracyPlot("ledger.prefetch.hit-rate", hitRate);
}

HistoryArchiveState
LedgerManagerImpl::storePersistentStateAndLedgerHeaderInDB(
    LedgerHeader const& header, bool appendToCheckpoint)
{
    ZoneScoped;

    releaseAssert(!isZero(xdrSha256(header)));
    auto& sess = mApp.getLedgerTxnRoot().getSession();

    if (mApp.getConfig().ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING.count() >
        0)
    {
        std::this_thread::sleep_for(
            mApp.getConfig().ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING);
    }

    LiveBucketList bl = mApp.getBucketManager().getLiveBucketList();
    // Store the current HAS in the database; this is really just to
    // checkpoint the bucketlist so we can survive a restart and re-attach
    // to the buckets.
    HistoryArchiveState has;
    if (protocolVersionStartsFrom(
            header.ledgerVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        auto hotBl = mApp.getBucketManager().getHotArchiveBucketList();
        has = HistoryArchiveState(header.ledgerSeq, bl, hotBl,
                                  mApp.getConfig().NETWORK_PASSPHRASE);
    }
    else
    {
        has = HistoryArchiveState(header.ledgerSeq, bl,
                                  mApp.getConfig().NETWORK_PASSPHRASE);
    }

    mApp.getPersistentState().setMainState(
        PersistentState::kHistoryArchiveState, has.toString(), sess);

    std::string headerEncoded = LedgerHeaderUtils::encodeHeader(header);
    mApp.getPersistentState().setMainState(
        PersistentState::kLastClosedLedgerHeader, headerEncoded, sess);

    if (appendToCheckpoint)
    {
        mApp.getHistoryManager().appendLedgerHeader(header);
    }

    return has;
}

// NB: This is a separate method so a testing subclass can override it.
std::optional<SorobanNetworkConfig>
LedgerManagerImpl::finalizeLedgerTxnChanges(
    ApplyLedgerStateSnapshot const& lclSnapshot, AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    LedgerHeader lh, uint32_t initialLedgerVers)
{
    ZoneScoped;
    // `ledgerApplied` protects this call with a mutex
    std::vector<LedgerEntry> initEntries, liveEntries;
    std::vector<LedgerKey> deadEntries;

    // Future for async hot archive batch operation.
    // addHotArchiveBatch modifies mHotArchiveBucketList which is independent
    // from mLiveBucketList (modified by addLiveBatch).
    std::future<void> hotArchiveBatchFuture;

    // Any V20 features must be behind initialLedgerVers check, see comment
    // in LedgerManagerImpl::ledgerApplied
    if (protocolVersionStartsFrom(initialLedgerVers, SOROBAN_PROTOCOL_VERSION))
    {
        // resolveBackgroundEvictionScan checks modified keys via direct O(1)
        // lookups in the LedgerTxn's EntryMap (isModifiedKey), avoiding the
        // need to build a full UnorderedSet of all modified keys.
        // It must be called at the right time _after_ all operations have
        // been applied, but _before_ evictions (ltx must not be sealed).
        auto evictedState =
            mApp.getBucketManager().resolveBackgroundEvictionScan(lclSnapshot,
                                                                  ltx);

        if (protocolVersionStartsFrom(
                initialLedgerVers,
                LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
        {
            std::vector<LedgerKey> restoredHotArchiveKeys;

            auto const& restoredHotArchiveKeyMap =
                ltx.getRestoredHotArchiveKeys();
            for (auto const& [key, entry] : restoredHotArchiveKeyMap)
            {
                // TTL keys are not recorded in the hot archive BucketList
                if (key.type() == CONTRACT_DATA || key.type() == CONTRACT_CODE)
                {
                    restoredHotArchiveKeys.push_back(key);
                }
            }

            mApp.getInvariantManager().checkOnLedgerCommit(
                lclSnapshot, evictedState.archivedEntries,
                evictedState.deletedKeys, restoredHotArchiveKeyMap,
                ltx.getRestoredLiveBucketListKeys());

            bool isP24UpgradeLedger =
                protocolVersionIsBefore(initialLedgerVers,
                                        ProtocolVersion::V_24) &&
                protocolVersionStartsFrom(lh.ledgerVersion,
                                          ProtocolVersion::V_24);
            if (isP24UpgradeLedger && gIsProductionNetwork)
            {
                p23_hot_archive_bug::addHotArchiveBatchWithP23HotArchiveFix(
                    ltx, mApp, lclSnapshot, lh, evictedState.archivedEntries,
                    restoredHotArchiveKeys);
            }
            else
            {
                // Launch addHotArchiveBatch asynchronously. It modifies
                // mHotArchiveBucketList which is independent from
                // mLiveBucketList, so it can run in parallel with addLiveBatch.
                auto& bucketManager = mApp.getBucketManager();
                auto archivedEntries = evictedState.archivedEntries;
                hotArchiveBatchFuture =
                    std::async(std::launch::async, [&bucketManager, this, lh,
                                                    archivedEntries,
                                                    restoredHotArchiveKeys]() {
                        ZoneScopedN("addHotArchiveBatch (async)");
                        bucketManager.addHotArchiveBatch(
                            mApp, lh, archivedEntries, restoredHotArchiveKeys);
                    });

                // Validate evicted entries against Protocol 23 corruption
                // data if configured
                if (mApp.getProtocol23CorruptionDataVerifier())
                {
                    mApp.getProtocol23CorruptionDataVerifier()
                        ->verifyArchivalOfCorruptedEntry(
                            evictedState, lclSnapshot, lh.ledgerSeq,
                            lh.ledgerVersion);
                }
            }
        }

        if (ledgerCloseMeta)
        {
            ledgerCloseMeta->populateEvictedEntries(evictedState);
        }

        mApplyState.evictFromModuleCache(lh.ledgerVersion, evictedState);

        // Update the Rust-owned SorobanState to reflect the eviction:
        // remove archived data/code entries from the in-memory map so
        // the next ledger's apply phase doesn't see stale TTLs for
        // entries that have left the live BucketList. Must run before
        // any read against InMemorySorobanState that might observe a
        // post-eviction state (specifically: the
        // RestoreFootprint footprint walk in the next apply phase).
        mApplyState.getInMemorySorobanStateForUpdate().evictEntries(
            evictedState.archivedEntries, evictedState.deletedKeys);

        // Subtle: we snapshot the state size *before* flushing the updated
        // entries into in-memory state (doing that after would be really
        // tricky, as we seal LTX before flushing). So the snapshot taken at
        // ledger `N` will have the state size for ledger `N - 1`. That
        // doesn't really change anything for the size accounting, but is
        // important to maintain as a protocol implementation detail.
        SorobanNetworkConfig::maybeSnapshotSorobanStateSize(
            lh.ledgerSeq, mApplyState.getSorobanInMemoryStateSize(), ltx, mApp);
    }
    std::optional<SorobanNetworkConfig> finalSorobanConfig;
    // NB: We're looking for the most up-to-date config at this point, so we
    // load it from ltx and we look at the current (potentially upgraded)
    // ledger version.
    if (protocolVersionStartsFrom(lh.ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        finalSorobanConfig =
            std::make_optional(SorobanNetworkConfig::loadFromLedger(ltx));
    }
    // NB: getAllEntries seals the ltx.
    ltx.getAllEntries(initEntries, liveEntries, deadEntries);

    // TODO(C9): the in-memory Soroban state update path used to dispatch
    // inMemoryState.updateState here on a worker thread. Per the design,
    // state mutation now happens inside the Rust apply phase
    // (apply_soroban_phase). Until that lands, the in-memory Soroban state
    // is not updated per ledger close — tests dependent on Soroban reads
    // will fail in this window.
    (void)finalSorobanConfig;

    mApplyState.addAnyContractsToModuleCache(lh.ledgerVersion, initEntries);
    mApplyState.addAnyContractsToModuleCache(lh.ledgerVersion, liveEntries);
    mApp.getBucketManager().addLiveBatch(mApp, lh, initEntries, liveEntries,
                                         deadEntries);
    // Wait for all async operations to complete before returning.
    if (hotArchiveBatchFuture.valid())
    {
        hotArchiveBatchFuture.get();
    }
    return finalSorobanConfig;
}

CompleteConstLedgerStatePtr
LedgerManagerImpl::sealLedgerTxnAndStoreInBucketsAndDB(
    ApplyLedgerStateSnapshot const& lclSnapshot, AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    uint32_t initialLedgerVers)
{
    ZoneScoped;

    JITTER_INJECT_DELAY();
    RecursiveMutexLocker lock(mLedgerStateMutex);
    JITTER_INJECT_DELAY();

    auto ledgerHeader = ltx.loadHeader().current();
    CLOG_TRACE(Ledger,
               "sealing ledger {} with version {}, sending to bucket list",
               ledgerHeader.ledgerSeq, ledgerHeader.ledgerVersion);

    // There is a subtle bug in the upgrade path that wasn't noticed until
    // protocol 20. For a ledger that upgrades from protocol vN to vN+1,
    // there are two different assumptions in different parts of the
    // ledger-close path:
    //   - In applyLedger we mostly treat the ledger as being on vN, eg.
    //   during
    //     tx apply and LCM construction.
    //   - In the final stage, when we call ledgerApplied, we pass vN+1
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
    auto sorobanConfig = finalizeLedgerTxnChanges(
        lclSnapshot, ltx, ledgerCloseMeta, ledgerHeader, initialLedgerVers);

    CompleteConstLedgerStatePtr res;
    ltx.unsealHeader([this, &res, sorobanConfig = std::move(sorobanConfig)](
                         LedgerHeader& lh) mutable {
        mApp.getBucketManager().snapshotLedger(lh);
        auto has = storePersistentStateAndLedgerHeaderInDB(
            lh, /* appendToCheckpoint */ true);
        res = advanceApplySnapshotAndMakeLedgerState(lh, has,
                                                     std::move(sorobanConfig));
    });

    releaseAssert(res);
    if (protocolVersionStartsFrom(
            initialLedgerVers, REUSABLE_SOROBAN_MODULE_CACHE_PROTOCOL_VERSION))
    {
        mApplyState.maybeRebuildModuleCache(initialLedgerVers);
    }

    return res;
}

void
LedgerManagerImpl::ApplyState::evictFromModuleCache(
    uint32_t ledgerVersion, EvictedStateVectors const& evictedState)
{
    ZoneScoped;
    assertCommittingPhase();
    std::vector<Hash> keys;
    for (auto const& key : evictedState.deletedKeys)
    {
        if (key.type() == CONTRACT_CODE)
        {
            keys.emplace_back(key.contractCode().hash);
        }
    }
    for (auto const& entry : evictedState.archivedEntries)
    {
        if (entry.data.type() == CONTRACT_CODE)
        {
            Hash const& hash = entry.data.contractCode().hash;
            keys.emplace_back(hash);
        }
    }
    if (keys.size() > 0)
    {
        CLOG_DEBUG(Ledger, "evicting {} modules from module cache",
                   keys.size());
        for (auto const& hash : keys)
        {
            CLOG_DEBUG(Ledger, "evicting {} from module cache", binToHex(hash));
            ::rust::Slice<uint8_t const> slice{hash.data(), hash.size()};
            mModuleCache->evict_contract_code(slice);
            getMetrics().mSorobanMetrics.mModuleCacheNumEntries.dec();
        }
    }
}

void
LedgerManagerImpl::ApplyState::addAnyContractsToModuleCache(
    uint32_t ledgerVersion, std::vector<LedgerEntry> const& le)
{
    ZoneScoped;
    assertWritablePhase();
    for (auto const& e : le)
    {
        if (e.data.type() == CONTRACT_CODE)
        {
            for (auto const& v : mModuleCacheProtocols)
            {
                if (v >= ledgerVersion)
                {
                    auto const& wasm = e.data.contractCode().code;
                    CLOG_DEBUG(Ledger,
                               "compiling wasm {} for protocol {} module cache",
                               binToHex(sha256(wasm)), v);
                    auto slice =
                        rust::Slice<uint8_t const>(wasm.data(), wasm.size());
                    getMetrics().mSorobanMetrics.mModuleCacheNumEntries.inc();
                    auto timer =
                        getMetrics()
                            .mSorobanMetrics.mModuleCompilationTime.TimeScope();
                    mModuleCache->compile(v, slice);
                }
            }
        }
    }
}
