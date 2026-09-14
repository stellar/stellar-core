// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/ImmutableLedgerView.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
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

LedgerHeaderWrapper::LedgerHeaderWrapper(
    std::shared_ptr<LedgerHeader const> header)
    : mHeader(std::move(header))
{
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

LedgerTxnView::LedgerTxnView(AbstractLedgerTxn& ltx,
                             SorobanNetworkConfig const* sorobanConfig)
    : mLedgerTxn(ltx), mSorobanConfig(sorobanConfig)
{
    if (protocolVersionStartsFrom(
            mLedgerTxn.loadHeader().current().ledgerVersion,
            SOROBAN_PROTOCOL_VERSION))
    {
        releaseAssertOrThrow(sorobanConfig != nullptr);
    }
}

LedgerTxnView::LedgerTxnView(AbstractLedgerTxn& ltx) : mLedgerTxn(ltx)
{
    if (protocolVersionStartsFrom(
            mLedgerTxn.loadHeader().current().ledgerVersion,
            SOROBAN_PROTOCOL_VERSION))
    {
        mLoadedConfig = SorobanNetworkConfig::loadFromLedger(mLedgerTxn);
        mSorobanConfig = &*mLoadedConfig;
    }
}

LedgerHeaderWrapper
LedgerTxnView::getLedgerHeader() const
{
    return LedgerHeaderWrapper(mLedgerTxn.loadHeader());
}

SorobanNetworkConfig const*
LedgerTxnView::getSorobanNetworkConfig() const
{
    return mSorobanConfig;
}

LedgerEntryWrapper
LedgerTxnView::getAccount(AccountID const& account) const
{
    return LedgerEntryWrapper(loadAccountWithoutRecord(mLedgerTxn, account));
}

LedgerEntryWrapper
LedgerTxnView::getAccount(LedgerHeaderWrapper const& header,
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
LedgerTxnView::getAccount(LedgerHeaderWrapper const& header,
                          TransactionFrame const& tx,
                          AccountID const& accountID) const
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        return LedgerEntryWrapper(
            tx.loadAccount(mLedgerTxn, header.getLedgerTxnHeader(), accountID));
    }
    return getAccount(accountID);
}

LedgerEntryWrapper
LedgerTxnView::load(LedgerKey const& key) const
{
    return LedgerEntryWrapper(mLedgerTxn.loadWithoutRecord(key));
}

void
ImmutableLedgerData::checkInvariant() const
{
    releaseAssert(mLastClosedHistoryArchiveState.currentLedger ==
                  mLastClosedLedgerHeader.header.ledgerSeq);
    releaseAssert(mLiveBucketData);
    releaseAssert(mHotArchiveBucketData);
}

ImmutableLedgerData::ImmutableLedgerData(
    LiveBucketList const& liveBL, HotArchiveBucketList const& hotArchiveBL,
    LedgerHeaderHistoryEntry const& lcl, HistoryArchiveState const& has,
    std::optional<SorobanNetworkConfig> sorobanConfig, MetricsRegistry& metrics)
    : mLiveBucketData(
          std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL))
    , mHotArchiveBucketData(
          std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(
              hotArchiveBL))
    , mLiveSnapshotMetrics(
          std::make_shared<BucketSnapshotMetrics<LiveBucket>>(metrics))
    , mHotArchiveSnapshotMetrics(
          std::make_shared<BucketSnapshotMetrics<HotArchiveBucket>>(metrics))
    , mLastClosedLedgerHeader(lcl)
    , mLastClosedHistoryArchiveState(has)
    , mSorobanConfig(std::move(sorobanConfig))
{
    if (!mSorobanConfig.has_value() &&
        protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        SearchableLiveBucketListSnapshot liveSnapshot(
            metrics, mLiveSnapshotMetrics, mLiveBucketData);
        mSorobanConfig.emplace(SorobanNetworkConfig::loadFromLedger(
            lcl.header.ledgerVersion, [&liveSnapshot](LedgerKey const& key) {
                return LedgerEntryWrapper(liveSnapshot.load(key));
            }));
    }

    checkInvariant();
}

SorobanNetworkConfig const&
ImmutableLedgerData::getSorobanConfig() const
{
    return mSorobanConfig.value();
}

bool
ImmutableLedgerData::hasSorobanConfig() const
{
    return mSorobanConfig.has_value();
}

LedgerHeaderHistoryEntry const&
ImmutableLedgerData::getLastClosedLedgerHeader() const
{
    return mLastClosedLedgerHeader;
}

HistoryArchiveState const&
ImmutableLedgerData::getLastClosedHistoryArchiveState() const
{
    return mLastClosedHistoryArchiveState;
}

ImmutableLedgerView::ImmutableLedgerView(ImmutableLedgerDataPtr state,
                                         MetricsRegistry& metrics)
    : mState(state)
    , mLiveSnapshot(metrics, state->mLiveSnapshotMetrics,
                    state->mLiveBucketData)
    , mHotArchiveSnapshot(metrics, state->mHotArchiveSnapshotMetrics,
                          state->mHotArchiveBucketData)
    , mMetrics(metrics)
{
}

ImmutableLedgerData const&
ImmutableLedgerView::getState() const
{
    releaseAssert(mState);
    return *mState;
}

LedgerHeaderWrapper
ImmutableLedgerView::getLedgerHeader() const
{
    // Avoid copying the header by aliasing the lifetime to mState shared_ptr
    return LedgerHeaderWrapper(std::shared_ptr<LedgerHeader const>(
        mState, &mState->getLastClosedLedgerHeader().header));
}

SorobanNetworkConfig const*
ImmutableLedgerView::getSorobanNetworkConfig() const
{
    return mState->hasSorobanConfig() ? &mState->getSorobanConfig() : nullptr;
}

uint32_t
ImmutableLedgerView::getLedgerSeq() const
{
    return mState->getLastClosedLedgerHeader().header.ledgerSeq;
}

LedgerEntryWrapper
ImmutableLedgerView::getAccount(AccountID const& account) const
{
    return LedgerEntryWrapper(loadLiveEntry(accountKey(account)));
}

LedgerEntryWrapper
ImmutableLedgerView::getAccount(LedgerHeaderWrapper const& header,
                                TransactionFrame const& tx) const
{
    return getAccount(tx.getSourceID());
}

LedgerEntryWrapper
ImmutableLedgerView::getAccount(LedgerHeaderWrapper const& header,
                                TransactionFrame const& tx,
                                AccountID const& AccountID) const
{
    return getAccount(AccountID);
}

LedgerEntryWrapper
ImmutableLedgerView::load(LedgerKey const& key) const
{
    return LedgerEntryWrapper(loadLiveEntry(key));
}
SorobanPreApplyLedgerView::SorobanPreApplyLedgerView(
    std::shared_ptr<LedgerHeader const> header,
    UpdatedEntryGetter getUpdatedEntry, ApplyLedgerView const& lclView)
    : mHeader(std::move(header))
    , mGetUpdatedEntry(std::move(getUpdatedEntry))
    , mLclView(lclView)
{
    releaseAssert(mGetUpdatedEntry);
}

LedgerHeaderWrapper
SorobanPreApplyLedgerView::getLedgerHeader() const
{
    return LedgerHeaderWrapper(mHeader);
}

SorobanNetworkConfig const*
SorobanPreApplyLedgerView::getSorobanNetworkConfig() const
{
    return mLclView.getSorobanNetworkConfig();
}

LedgerEntryWrapper
SorobanPreApplyLedgerView::getAccount(AccountID const& account) const
{
    return load(accountKey(account));
}

LedgerEntryWrapper
SorobanPreApplyLedgerView::getAccount(LedgerHeaderWrapper const& header,
                                      TransactionFrame const& tx) const
{
    return getAccount(tx.getSourceID());
}

LedgerEntryWrapper
SorobanPreApplyLedgerView::getAccount(LedgerHeaderWrapper const& header,
                                      TransactionFrame const& tx,
                                      AccountID const& accountID) const
{
    return getAccount(accountID);
}

LedgerEntryWrapper
SorobanPreApplyLedgerView::load(LedgerKey const& key) const
{
    auto updatedEntry = mGetUpdatedEntry(key);
    if (updatedEntry)
    {
        // Modified in this ledger, so this is the authoritative version.
        // A null entry means it has been deleted.
        return LedgerEntryWrapper(*updatedEntry);
    }
    // Not modified in this ledger, so the last closed ledger snapshot is
    // up to date.
    return LedgerEntryWrapper(mLclView.loadLiveEntry(key));
}

// === Live BucketList wrapper methods ===

std::shared_ptr<LedgerEntry const>
ImmutableLedgerView::loadLiveEntry(LedgerKey const& k) const
{
    return mLiveSnapshot.load(k);
}

std::vector<LedgerEntry>
ImmutableLedgerView::loadLiveKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    std::string const& label) const
{
    return mLiveSnapshot.loadKeys(inKeys, label);
}

std::vector<LedgerEntry>
ImmutableLedgerView::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    return mLiveSnapshot.loadPoolShareTrustLinesByAccountAndAsset(accountID,
                                                                  asset);
}

std::vector<InflationWinner>
ImmutableLedgerView::loadInflationWinners(size_t maxWinners,
                                          int64_t minBalance) const
{
    return mLiveSnapshot.loadInflationWinners(maxWinners, minBalance);
}

std::unique_ptr<EvictionResultCandidates>
ImmutableLedgerView::scanForEviction(uint32_t ledgerSeq,
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
ImmutableLedgerView::scanLiveEntriesOfType(
    LedgerEntryType type,
    std::function<Loop(BucketEntry const&)> callback) const
{
    mLiveSnapshot.scanForEntriesOfType(type, std::move(callback));
}

void
ImmutableLedgerView::scanCurrentLiveEntriesOfType(
    LedgerEntryType type,
    std::function<void(LedgerEntry const&, LedgerKey const&)> callback) const
{
    mLiveSnapshot.scanForLiveEntriesOfType(type, std::move(callback));
}

// === Hot Archive BucketList wrapper methods ===

std::shared_ptr<HotArchiveBucketEntry const>
ImmutableLedgerView::loadArchiveEntry(LedgerKey const& k) const
{
    return mHotArchiveSnapshot.load(k);
}

std::vector<HotArchiveBucketEntry>
ImmutableLedgerView::loadArchiveKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    std::string const& label) const
{
    return mHotArchiveSnapshot.loadKeys(inKeys, label);
}

void
ImmutableLedgerView::scanAllArchiveEntries(
    std::function<Loop(HotArchiveBucketEntry const&)> callback) const
{
    mHotArchiveSnapshot.scanAllEntries(std::move(callback));
}

ApplyLedgerView::ApplyLedgerView(ImmutableLedgerDataPtr state,
                                 MetricsRegistry& metrics)
    : ImmutableLedgerView(std::move(state), metrics)
{
}
}
