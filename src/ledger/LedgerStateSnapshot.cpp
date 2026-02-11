// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateSnapshot.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

LedgerEntryWrapper::LedgerEntryWrapper(ConstLedgerTxnEntry&& entry)
    : mEntry(std::move(entry))
{
}

LedgerEntryWrapper::LedgerEntryWrapper(LedgerTxnEntry&& entry)
    : mEntry(std::move(entry))
{
}

LedgerEntryWrapper::LedgerEntryWrapper(std::shared_ptr<LedgerEntry const> entry)
    : mEntry(entry)
{
}

LedgerEntry const&
LedgerEntryWrapper::current() const
{
    switch (mEntry.index())
    {
    case 0:
        return std::get<0>(mEntry).current();
    case 1:
        return std::get<1>(mEntry).current();
    case 2:
    {
        auto res = std::get<2>(mEntry);
        releaseAssertOrThrow(res);
        return *res;
    }
    default:
        throw std::runtime_error("Invalid LedgerEntryWrapper index");
    }
}

LedgerEntryWrapper::
operator bool() const
{
    switch (mEntry.index())
    {
    case 0:
        return static_cast<bool>(std::get<0>(mEntry));
    case 1:
        return static_cast<bool>(std::get<1>(mEntry));
    case 2:
        return static_cast<bool>(std::get<2>(mEntry));
    default:
        throw std::runtime_error("Invalid LedgerEntryWrapper index");
    }
}

LedgerHeaderWrapper::LedgerHeaderWrapper(LedgerTxnHeader&& header)
    : mHeader(std::move(header))
{
}

LedgerHeaderWrapper::LedgerHeaderWrapper(std::shared_ptr<LedgerHeader> header)
    : mHeader(header)
{
}

LedgerHeader&
LedgerHeaderWrapper::currentToModify()
{
    switch (mHeader.index())
    {
    case 0:
        return std::get<0>(mHeader).current();
    case 1:
        return *std::get<1>(mHeader);
    default:
        throw std::runtime_error("Invalid LedgerHeaderWrapper index");
    }
}

LedgerHeader const&
LedgerHeaderWrapper::current() const
{
    switch (mHeader.index())
    {
    case 0:
        return std::get<0>(mHeader).current();
    case 1:
        return *std::get<1>(mHeader);
    default:
        throw std::runtime_error("Invalid LedgerHeaderWrapper index");
    }
}

LedgerTxnReadOnly::LedgerTxnReadOnly(AbstractLedgerTxn& ltx) : mLedgerTxn(ltx)
{
}

LedgerTxnReadOnly::~LedgerTxnReadOnly()
{
}

LedgerHeaderWrapper
LedgerTxnReadOnly::getLedgerHeader() const
{
    return LedgerHeaderWrapper(mLedgerTxn.loadHeader());
}

LedgerEntryWrapper
LedgerTxnReadOnly::getAccount(AccountID const& account) const
{
    return LedgerEntryWrapper(loadAccountWithoutRecord(mLedgerTxn, account));
}

LedgerEntryWrapper
LedgerTxnReadOnly::getAccount(LedgerHeaderWrapper const& header,
                              TransactionFrame const& tx) const
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        return LedgerEntryWrapper(
            tx.loadSourceAccount(mLedgerTxn, header.getLedgerTxnHeader()));
    }

    return getAccount(tx.getSourceID());
}

LedgerEntryWrapper
LedgerTxnReadOnly::getAccount(LedgerHeaderWrapper const& header,
                              TransactionFrame const& tx,
                              AccountID const& account) const
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        return LedgerEntryWrapper(
            tx.loadAccount(mLedgerTxn, header.getLedgerTxnHeader(), account));
    }

    return getAccount(account);
}

LedgerEntryWrapper
LedgerTxnReadOnly::load(LedgerKey const& key) const
{
    return LedgerEntryWrapper(mLedgerTxn.loadWithoutRecord(key));
}

void
LedgerTxnReadOnly::executeWithMaybeInnerSnapshot(
    std::function<void(LedgerSnapshot const& ls)> f) const
{
    LedgerTxn inner(mLedgerTxn);
    LedgerSnapshot lsg(inner);
    return f(lsg);
}

BucketSnapshotState::BucketSnapshotState(LedgerStateSnapshot const& snap)
    : mSnapshot(snap)
    , mLedgerHeader(LedgerHeaderWrapper(
          std::make_shared<LedgerHeader>(snap.getLedgerHeader())))
{
}

BucketSnapshotState::~BucketSnapshotState()
{
}

LedgerHeaderWrapper
BucketSnapshotState::getLedgerHeader() const
{
    return LedgerHeaderWrapper(std::get<1>(mLedgerHeader.mHeader));
}

LedgerEntryWrapper
BucketSnapshotState::getAccount(AccountID const& account) const
{
    return LedgerEntryWrapper(mSnapshot.loadLiveEntry(accountKey(account)));
}

LedgerEntryWrapper
BucketSnapshotState::getAccount(LedgerHeaderWrapper const& header,
                                TransactionFrame const& tx) const
{
    return getAccount(tx.getSourceID());
}

LedgerEntryWrapper
BucketSnapshotState::getAccount(LedgerHeaderWrapper const& header,
                                TransactionFrame const& tx,
                                AccountID const& AccountID) const
{
    return getAccount(AccountID);
}

LedgerEntryWrapper
BucketSnapshotState::load(LedgerKey const& key) const
{
    return LedgerEntryWrapper(mSnapshot.loadLiveEntry(key));
}

void
BucketSnapshotState::executeWithMaybeInnerSnapshot(
    std::function<void(LedgerSnapshot const& ls)> f) const
{
    throw std::runtime_error(
        "BucketSnapshotState::executeWithMaybeInnerSnapshot is illegal: "
        "BucketSnapshotState has no nested snapshots");
}

LedgerSnapshot::LedgerSnapshot(AbstractLedgerTxn& ltx)
    : mGetter(std::make_unique<LedgerTxnReadOnly>(ltx))
{
}

LedgerSnapshot::LedgerSnapshot(Application& app)
{
    releaseAssert(threadIsMain());
#ifdef BUILD_TESTS
    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        // Legacy read-only SQL transaction
        mLegacyLedgerTxn = std::make_unique<LedgerTxn>(
            app.getLedgerTxnRoot(), /* shouldUpdateLastModified*/ false,
            TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
        mGetter = std::make_unique<LedgerTxnReadOnly>(*mLegacyLedgerTxn);
    }
    else
#endif
    {
        auto snap = app.getBucketManager()
                        .getBucketSnapshotManager()
                        .copyLedgerStateSnapshot();
        mGetter = std::make_unique<BucketSnapshotState>(snap);
    }
}

LedgerSnapshot::LedgerSnapshot(LedgerStateSnapshot const& snap)
    : mGetter(std::make_unique<BucketSnapshotState>(snap))
{
}

LedgerHeaderWrapper
LedgerSnapshot::getLedgerHeader() const
{
    return mGetter->getLedgerHeader();
}

LedgerEntryWrapper
LedgerSnapshot::getAccount(AccountID const& account) const
{
    return mGetter->getAccount(account);
}

LedgerEntryWrapper
LedgerSnapshot::load(LedgerKey const& key) const
{
    return mGetter->load(key);
}

void
LedgerSnapshot::executeWithMaybeInnerSnapshot(
    std::function<void(LedgerSnapshot const& ls)> f) const
{
    return mGetter->executeWithMaybeInnerSnapshot(f);
}

void
CompleteConstLedgerState::checkInvariant() const
{
    releaseAssert(mLastClosedHistoryArchiveState.currentLedger ==
                  mLastClosedLedgerHeader.header.ledgerSeq);
    // Initial placeholder state (ledgerSeq 0) may have null bucket data
    // TODO: Get rid of this since it's kinda sketchy
    if (mLastClosedLedgerHeader.header.ledgerSeq > 0)
    {
        releaseAssert(mLiveBucketData);
        releaseAssert(mHotArchiveBucketData);
    }
}

CompleteConstLedgerState::CompleteConstLedgerState(
    BucketListBase<LiveBucket> const& liveBL,
    BucketListBase<HotArchiveBucket> const& hotArchiveBL,
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
        liveHistorical,
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
        hotArchiveHistorical,
    LedgerHeaderHistoryEntry const& lastClosedLedgerHeader,
    HistoryArchiveState const& lastClosedHistoryArchiveState,
    MetricsRegistry& metrics)
    : mLiveBucketData(
          std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL))
    , mLiveHistoricalSnapshots(std::move(liveHistorical))
    , mHotArchiveBucketData(
          std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(
              hotArchiveBL))
    , mHotArchiveHistoricalSnapshots(std::move(hotArchiveHistorical))
    , mSorobanConfig(
          protocolVersionStartsFrom(lastClosedLedgerHeader.header.ledgerVersion,
                                    SOROBAN_PROTOCOL_VERSION)
              ? std::make_optional([&]() {
                    // Create a temporary CompleteConstLedgerState (without
                    // Soroban config) to serve as the canonical state for the
                    // LedgerStateSnapshot used during config loading.
                    // TODO: Sketchy, please remove
                    auto tempState =
                        std::shared_ptr<CompleteConstLedgerState const>(
                            new CompleteConstLedgerState(
                                mLiveBucketData, mLiveHistoricalSnapshots,
                                mHotArchiveBucketData,
                                mHotArchiveHistoricalSnapshots,
                                lastClosedLedgerHeader,
                                lastClosedHistoryArchiveState));
                    LedgerStateSnapshot tempSnap(tempState, metrics);
                    LedgerSnapshot ls(tempSnap);
                    return SorobanNetworkConfig::loadFromLedger(ls);
                }())
              : std::nullopt)
    , mLastClosedLedgerHeader(lastClosedLedgerHeader)
    , mLastClosedHistoryArchiveState(lastClosedHistoryArchiveState)
{
    checkInvariant();
}

CompleteConstLedgerState::CompleteConstLedgerState(
    LedgerHeaderHistoryEntry const& lastClosedLedgerHeader,
    HistoryArchiveState const& lastClosedHistoryArchiveState)
    : mLiveBucketData(nullptr)
    , mLiveHistoricalSnapshots()
    , mHotArchiveBucketData(nullptr)
    , mHotArchiveHistoricalSnapshots()
    , mSorobanConfig(std::nullopt)
    , mLastClosedLedgerHeader(lastClosedLedgerHeader)
    , mLastClosedHistoryArchiveState(lastClosedHistoryArchiveState)
{
    checkInvariant();
}

CompleteConstLedgerState::CompleteConstLedgerState(
    std::shared_ptr<BucketListSnapshotData<LiveBucket> const> liveBucketData,
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
        liveHistorical,
    std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>
        hotArchiveBucketData,
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
        hotArchiveHistorical,
    LedgerHeaderHistoryEntry const& lastClosedLedgerHeader,
    HistoryArchiveState const& lastClosedHistoryArchiveState)
    : mLiveBucketData(std::move(liveBucketData))
    , mLiveHistoricalSnapshots(std::move(liveHistorical))
    , mHotArchiveBucketData(std::move(hotArchiveBucketData))
    , mHotArchiveHistoricalSnapshots(std::move(hotArchiveHistorical))
    , mSorobanConfig(std::nullopt)
    , mLastClosedLedgerHeader(lastClosedLedgerHeader)
    , mLastClosedHistoryArchiveState(lastClosedHistoryArchiveState)
{
    checkInvariant();
}

SorobanNetworkConfig const&
CompleteConstLedgerState::getSorobanConfig() const
{
    return mSorobanConfig.value();
}

bool
CompleteConstLedgerState::hasSorobanConfig() const
{
    return mSorobanConfig.has_value();
}

LedgerHeaderHistoryEntry const&
CompleteConstLedgerState::getLastClosedLedgerHeader() const
{
    return mLastClosedLedgerHeader;
}

HistoryArchiveState const&
CompleteConstLedgerState::getLastClosedHistoryArchiveState() const
{
    return mLastClosedHistoryArchiveState;
}

LedgerStateSnapshot::LedgerStateSnapshot(CompleteConstLedgerStatePtr state,
                                         MetricsRegistry& metrics)
    : mState(state)
    , mLiveSnapshot(metrics, state->mLiveBucketData,
                    state->mLiveHistoricalSnapshots,
                    state->mLastClosedLedgerHeader.header.ledgerSeq)
    , mHotArchiveSnapshot(metrics, state->mHotArchiveBucketData,
                          state->mHotArchiveHistoricalSnapshots,
                          state->mLastClosedLedgerHeader.header.ledgerSeq)
    , mMetrics(metrics)
{
}

CompleteConstLedgerState const&
LedgerStateSnapshot::getState() const
{
    releaseAssert(mState);
    return *mState;
}

LedgerHeader const&
LedgerStateSnapshot::getLedgerHeader() const
{
    return mState->getLastClosedLedgerHeader().header;
}

uint32_t
LedgerStateSnapshot::getLedgerSeq() const
{
    return mState->getLastClosedLedgerHeader().header.ledgerSeq;
}

// === Live BucketList wrapper methods ===

std::shared_ptr<LedgerEntry const>
LedgerStateSnapshot::loadLiveEntry(LedgerKey const& k) const
{
    return mLiveSnapshot.load(k);
}

std::vector<LedgerEntry>
LedgerStateSnapshot::loadLiveKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    std::string const& label) const
{
    return mLiveSnapshot.loadKeys(inKeys, label);
}

std::optional<std::vector<LedgerEntry>>
LedgerStateSnapshot::loadLiveKeysFromLedger(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    uint32_t ledgerSeq) const
{
    return mLiveSnapshot.loadKeysFromLedger(inKeys, ledgerSeq);
}

std::vector<LedgerEntry>
LedgerStateSnapshot::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    return mLiveSnapshot.loadPoolShareTrustLinesByAccountAndAsset(accountID,
                                                                  asset);
}

std::vector<InflationWinner>
LedgerStateSnapshot::loadInflationWinners(size_t maxWinners,
                                          int64_t minBalance) const
{
    return mLiveSnapshot.loadInflationWinners(maxWinners, minBalance);
}

std::unique_ptr<EvictionResultCandidates>
LedgerStateSnapshot::scanForEviction(uint32_t ledgerSeq,
                                     EvictionMetrics& metrics,
                                     EvictionIterator iter,
                                     std::shared_ptr<EvictionStatistics> stats,
                                     StateArchivalSettings const& sas,
                                     uint32_t ledgerVers) const
{
    return mLiveSnapshot.scanForEviction(ledgerSeq, metrics, std::move(iter),
                                         std::move(stats), sas, ledgerVers);
}

void
LedgerStateSnapshot::scanLiveEntriesOfType(
    LedgerEntryType type,
    std::function<Loop(BucketEntry const&)> callback) const
{
    mLiveSnapshot.scanForEntriesOfType(type, std::move(callback));
}

// === Hot Archive BucketList wrapper methods ===

std::shared_ptr<HotArchiveBucketEntry const>
LedgerStateSnapshot::loadArchiveEntry(LedgerKey const& k) const
{
    return mHotArchiveSnapshot.load(k);
}

std::vector<HotArchiveBucketEntry>
LedgerStateSnapshot::loadArchiveKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const
{
    return mHotArchiveSnapshot.loadKeys(inKeys);
}

std::optional<std::vector<HotArchiveBucketEntry>>
LedgerStateSnapshot::loadArchiveKeysFromLedger(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    uint32_t ledgerSeq) const
{
    return mHotArchiveSnapshot.loadKeysFromLedger(inKeys, ledgerSeq);
}

void
LedgerStateSnapshot::scanAllArchiveEntries(
    std::function<Loop(HotArchiveBucketEntry const&)> callback) const
{
    mHotArchiveSnapshot.scanAllEntries(std::move(callback));
}
}
