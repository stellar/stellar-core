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
    std::function<void(CheckValidLedgerViewWrapper const& ledgerView)> f) const
{
    LedgerTxn inner(mLedgerTxn);
    CheckValidLedgerViewWrapper ledgerView(inner);
    return f(ledgerView);
}

CheckValidLedgerViewWrapper::CheckValidLedgerViewWrapper(AbstractLedgerTxn& ltx)
    : mGetter(std::make_unique<LedgerTxnReadOnly>(ltx))
{
}

CheckValidLedgerViewWrapper::CheckValidLedgerViewWrapper(Application& app)
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
        mGetter = std::make_unique<ImmutableLedgerView>(
            app.getLedgerManager().copyImmutableLedgerView());
    }
}

CheckValidLedgerViewWrapper::CheckValidLedgerViewWrapper(
    ImmutableLedgerView const& ledgerView)
    : mGetter(std::make_unique<ImmutableLedgerView>(ledgerView))
{
}

LedgerHeaderWrapper
CheckValidLedgerViewWrapper::getLedgerHeader() const
{
    return mGetter->getLedgerHeader();
}

LedgerEntryWrapper
CheckValidLedgerViewWrapper::getAccount(AccountID const& account) const
{
    return mGetter->getAccount(account);
}

LedgerEntryWrapper
CheckValidLedgerViewWrapper::load(LedgerKey const& key) const
{
    return mGetter->load(key);
}

void
CheckValidLedgerViewWrapper::executeWithMaybeInnerSnapshot(
    std::function<void(CheckValidLedgerViewWrapper const& ledgerView)> f) const
{
    return mGetter->executeWithMaybeInnerSnapshot(f);
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
    std::optional<SorobanNetworkConfig> sorobanConfig)
    : mLiveBucketData(
          std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL))
    , mHotArchiveBucketData(
          std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(
              hotArchiveBL))
    , mSorobanConfig(std::move(sorobanConfig))
    , mLastClosedLedgerHeader(lcl)
    , mLastClosedHistoryArchiveState(has)
{
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

ImmutableLedgerDataPtr
ImmutableLedgerData::createAndMaybeLoadConfig(
    LiveBucketList const& liveBL, HotArchiveBucketList const& hotArchiveBL,
    LedgerHeaderHistoryEntry const& lcl, HistoryArchiveState const& has,
    MetricsRegistry& metrics)
{
    std::optional<SorobanNetworkConfig> sorobanConfig;
    if (protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        // Bootstrap: build a lightweight temporary state just to load config
        // from the current live bucket list.
        auto tempState = std::make_shared<ImmutableLedgerData>(
            liveBL, hotArchiveBL, lcl, has, /*sorobanConfig*/ std::nullopt);
        ImmutableLedgerView tempView(tempState, metrics);
        sorobanConfig = SorobanNetworkConfig::loadFromLedger(tempView);
    }
    return std::make_shared<ImmutableLedgerData>(
        liveBL, hotArchiveBL, lcl, has, std::move(sorobanConfig));
}

ImmutableLedgerView::ImmutableLedgerView(ImmutableLedgerDataPtr state,
                                         MetricsRegistry& metrics)
    : mState(state)
    , mLiveSnapshot(metrics, state->mLiveBucketData)
    , mHotArchiveSnapshot(metrics, state->mHotArchiveBucketData)
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

void
ImmutableLedgerView::executeWithMaybeInnerSnapshot(
    std::function<void(CheckValidLedgerViewWrapper const& ledgerView)> f) const
{
    throw std::runtime_error(
        "ImmutableLedgerView::executeWithMaybeInnerSnapshot is illegal: "
        "ImmutableLedgerView has no nested snapshots");
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

// === Hot Archive BucketList wrapper methods ===

std::shared_ptr<HotArchiveBucketEntry const>
ImmutableLedgerView::loadArchiveEntry(LedgerKey const& k) const
{
    return mHotArchiveSnapshot.load(k);
}

std::vector<HotArchiveBucketEntry>
ImmutableLedgerView::loadArchiveKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const
{
    return mHotArchiveSnapshot.loadKeys(inKeys);
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
