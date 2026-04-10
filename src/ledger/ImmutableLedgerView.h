// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketListSnapshot.h"
#include "history/HistoryArchive.h"
#include "ledger/LedgerTxn.h"
#include "ledger/NetworkConfig.h"
#include "util/NonCopyable.h"
#include <functional>
#include <variant>

namespace stellar
{

class Application;
class TransactionFrame;
class CheckValidLedgerViewWrapper;
class ApplyLedgerView;
class ImmutableLedgerData;
class EvictionStatistics;
struct EvictionMetrics;
struct EvictionResultCandidates;
struct InflationWinner;
struct StateArchivalSettings;
class LiveBucketList;
class HotArchiveBucketList;

// NB: we can't use unique_ptr here, because this object gets passed to a
// lambda, and std::function requires its callable to be copyable (C++23 fixes
// this with std::move_only_function, but we're not there yet).
using ImmutableLedgerDataPtr = std::shared_ptr<ImmutableLedgerData const>;

// A unified ledger entry interface that supports LedgerEntry representations
// for both legacy SQL and BucketList snapshots. When working with LedgerTxn,
// using plain LedgerEntry is not safe, as it might be modified or invalidated
// by a nested transaction. To address this, the LedgerTxnEntry and
// ConstLedgerTxnEntry abstractions should be preserved, allowing bucket
// snapshot LedgerEntries to support the same interface.
class LedgerEntryWrapper
{
    // Either hold a reference or a pointer to the entry
    std::variant<LedgerTxnEntry, ConstLedgerTxnEntry,
                 std::shared_ptr<LedgerEntry const>>
        mEntry;

  public:
    explicit LedgerEntryWrapper(ConstLedgerTxnEntry&& entry);
    explicit LedgerEntryWrapper(LedgerTxnEntry&& entry);
    explicit LedgerEntryWrapper(std::shared_ptr<LedgerEntry const> entry);
    LedgerEntry const& current() const;
    operator bool() const;
};

// A unified ledger header access interface, similar to
// LedgerEntryWrapper. Just like LedgerEntryWrapper, this class is purely
// cosmetic for BucketList snapshots, since those are immutable.
class LedgerHeaderWrapper
{
    std::variant<LedgerTxnHeader, std::shared_ptr<LedgerHeader const>> mHeader;

  public:
    explicit LedgerHeaderWrapper(LedgerTxnHeader&& header);
    explicit LedgerHeaderWrapper(std::shared_ptr<LedgerHeader const> header);
    LedgerHeader const& current() const;
    LedgerTxnHeader const&
    getLedgerTxnHeader() const
    {
        releaseAssert(std::holds_alternative<LedgerTxnHeader>(mHeader));
        return std::get<0>(mHeader);
    }
};

// A unified interface for read-only ledger state snapshot.
// Supports SQL (via read-only LedgerTxn), as well as BucketList snapshots.
class AbstractLedgerView
{
  public:
    virtual ~AbstractLedgerView() = default;
    virtual LedgerHeaderWrapper getLedgerHeader() const = 0;
    virtual LedgerEntryWrapper getAccount(AccountID const& account) const = 0;
    virtual LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                          TransactionFrame const& tx) const = 0;
    virtual LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                          TransactionFrame const& tx,
                                          AccountID const& AccountID) const = 0;
    virtual LedgerEntryWrapper load(LedgerKey const& key) const = 0;
    // Execute a function with a nested snapshot, if supported. This is needed
    // to support the replay of old buggy protocols (<8), see
    // `TransactionFrame::loadSourceAccount`
    virtual void executeWithMaybeInnerSnapshot(
        std::function<void(CheckValidLedgerViewWrapper const&)> f) const = 0;
};

// A concrete implementation of read-only SQL snapshot wrapper
class LedgerTxnReadOnly : public AbstractLedgerView
{
    // Callers are expected to manage `AbstractLedgerTxn` themselves.
    // LedgerTxnReadOnly guarantees that LedgerTxn, LedgerTxnHeader and
    // LedgerTxnEntry referenced all remain valid.
    AbstractLedgerTxn& mLedgerTxn;

  public:
    LedgerTxnReadOnly(AbstractLedgerTxn& ltx);
    ~LedgerTxnReadOnly() override;
    LedgerHeaderWrapper getLedgerHeader() const override;
    LedgerEntryWrapper getAccount(AccountID const& account) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx,
                                  AccountID const& AccountID) const override;
    LedgerEntryWrapper load(LedgerKey const& key) const override;
    void executeWithMaybeInnerSnapshot(
        std::function<void(CheckValidLedgerViewWrapper const&)> f)
        const override;
};

// A copyable value type that provides searchable access to a
// ImmutableLedgerData. Each instance maintains its own file stream cache
// for bucket I/O. Multiple ImmutableLedgerView instances can safely wrap the
// same ImmutableLedgerData.
class ImmutableLedgerView : public virtual AbstractLedgerView
{
    std::shared_ptr<ImmutableLedgerData const> mState;
    SearchableLiveBucketListSnapshot mLiveSnapshot;
    SearchableHotArchiveBucketListSnapshot mHotArchiveSnapshot;
    std::reference_wrapper<MetricsRegistry> mMetrics;

    friend class ImmutableLedgerData;

  public:
    // Construct from ImmutableLedgerData
    explicit ImmutableLedgerView(ImmutableLedgerDataPtr state,
                                 MetricsRegistry& metrics);

    ImmutableLedgerData const& getState() const;
    LedgerHeaderWrapper getLedgerHeader() const override;
    uint32_t getLedgerSeq() const;

    // === AbstractLedgerView overrides ===
    LedgerEntryWrapper getAccount(AccountID const& account) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx,
                                  AccountID const& AccountID) const override;
    LedgerEntryWrapper load(LedgerKey const& key) const override;
    void executeWithMaybeInnerSnapshot(
        std::function<void(CheckValidLedgerViewWrapper const&)> f)
        const override;

    // === Live BucketList methods ===
    std::shared_ptr<LedgerEntry const> loadLiveEntry(LedgerKey const& k) const;
    std::vector<LedgerEntry>
    loadLiveKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                 std::string const& label) const;
    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset) const;
    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;
    std::unique_ptr<EvictionResultCandidates> scanForEviction(
        uint32_t ledgerSeq, EvictionMetrics& metrics, EvictionIterator iter,
        std::shared_ptr<EvictionStatistics> stats,
        StateArchivalSettings const& sas, uint32_t ledgerVers) const;
    void scanLiveEntriesOfType(
        LedgerEntryType type,
        std::function<Loop(BucketEntry const&)> callback) const;

    // === Hot Archive BucketList methods ===
    std::shared_ptr<HotArchiveBucketEntry const>
    loadArchiveEntry(LedgerKey const& k) const;
    std::vector<HotArchiveBucketEntry>
    loadArchiveKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const;
    void scanAllArchiveEntries(
        std::function<Loop(HotArchiveBucketEntry const&)> callback) const;
};

// A strong typedef for ImmutableLedgerView that represents a snapshot used
// during apply time. This is identical to ImmutableLedgerView in practice, but
// is a distinct type to prevent accidental interchange between apply-time
// snapshots and other snapshots (e.g., from mLastClosedLedgerState).
class ApplyLedgerView : private ImmutableLedgerView,
                        public virtual AbstractLedgerView
{
  public:
    explicit ApplyLedgerView(ImmutableLedgerDataPtr state,
                             MetricsRegistry& metrics);

    using ImmutableLedgerView::executeWithMaybeInnerSnapshot;
    using ImmutableLedgerView::getAccount;
    using ImmutableLedgerView::getLedgerHeader;
    using ImmutableLedgerView::getLedgerSeq;
    using ImmutableLedgerView::getState;
    using ImmutableLedgerView::load;
    using ImmutableLedgerView::loadArchiveEntry;
    using ImmutableLedgerView::loadArchiveKeys;
    using ImmutableLedgerView::loadInflationWinners;
    using ImmutableLedgerView::loadLiveEntry;
    using ImmutableLedgerView::loadLiveKeys;
    using ImmutableLedgerView::loadPoolShareTrustLinesByAccountAndAsset;
    using ImmutableLedgerView::scanAllArchiveEntries;
    using ImmutableLedgerView::scanForEviction;
    using ImmutableLedgerView::scanLiveEntriesOfType;
};

// A helper class to create and query read-only snapshots
// Automatically decides whether to create a BucketList (recommended), or SQL
// snapshot (deprecated, but currently supported)
// NOTE: CheckValidLedgerViewWrapper is meant to be short-lived, and should not
// be persisted across _different_ ledgers, as the state under the hood might
// change. Users are expected to construct a new CheckValidLedgerViewWrapper
// each time they want to query ledger state.
class CheckValidLedgerViewWrapper : public NonMovableOrCopyable
{
    std::unique_ptr<AbstractLedgerView const> mGetter;
    std::unique_ptr<LedgerTxn> mLegacyLedgerTxn;

  public:
    CheckValidLedgerViewWrapper(AbstractLedgerTxn& ltx);
    CheckValidLedgerViewWrapper(Application& app);
    explicit CheckValidLedgerViewWrapper(ImmutableLedgerView const& ledgerView);
#ifdef BUILD_TESTS
    // Set by overlay-only mode call sites so commonValid skips the seqnum
    // equality check: on-disk seqnums are frozen at genesis while
    // LoadGenerator keeps advancing its local counters, so every tx after the
    // first would otherwise fail isBadSeq.
    bool mSkipSeqNumCheck{false};
#endif
    LedgerHeaderWrapper getLedgerHeader() const;
    LedgerEntryWrapper getAccount(AccountID const& account) const;
    LedgerEntryWrapper
    getAccount(LedgerHeaderWrapper const& header,
               TransactionFrame const& tx) const
    {
        return mGetter->getAccount(header, tx);
    }
    LedgerEntryWrapper
    getAccount(LedgerHeaderWrapper const& header, TransactionFrame const& tx,
               AccountID const& AccountID) const
    {
        return mGetter->getAccount(header, tx, AccountID);
    }
    LedgerEntryWrapper load(LedgerKey const& key) const;

    // Execute a function with a nested snapshot, if supported. This is needed
    // to support the replay of old buggy protocols (<8), see
    // `TransactionFrame::loadSourceAccount`
    void executeWithMaybeInnerSnapshot(
        std::function<void(CheckValidLedgerViewWrapper const&)> f) const;
};

// Immutable wrapper for a complete ledger state snapshot.
// This object provides read-only access to all components of a full ledger
// state at a specific ledger sequence. All components are instantiated together
// and cannot be modified after construction.
//
// The five components included are:
// 1. BucketList snapshot – a read-only view of the live bucket list at ledger N
// 2. Hot Archive snapshot – a read-only view of the hot archive at ledger N
// 3. Soroban network configuration – the configuration at ledger N
// 4. Last closed ledger header – the header of ledger N
// 5. Last closed history archive state – the archive state at ledger N
//
// All member objects are immutable. Getters return const references;
// however, these references should not be assumed to have long lifetimes.
// A new ledger closure may cause LedgerManager to replace the current
// ImmutableLedgerData instance.
class ImmutableLedgerData : public NonMovableOrCopyable
{
  private:
    // Raw immutable bucket data for the live and hot archive bucket lists
    std::shared_ptr<BucketListSnapshotData<LiveBucket> const> const
        mLiveBucketData;
    std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const> const
        mHotArchiveBucketData;

    std::optional<SorobanNetworkConfig const> const mSorobanConfig;
    LedgerHeaderHistoryEntry const mLastClosedLedgerHeader;
    HistoryArchiveState const mLastClosedHistoryArchiveState;

    void checkInvariant() const;

    friend class ImmutableLedgerView;

  public:
    // Construct a new immutable ledger state snapshot.
    // sorobanConfig is nullopt for pre-Soroban protocol versions, or when
    // building the empty initial state at startup.
    ImmutableLedgerData(LiveBucketList const& liveBL,
                        HotArchiveBucketList const& hotArchiveBL,
                        LedgerHeaderHistoryEntry const& lcl,
                        HistoryArchiveState const& has,
                        std::optional<SorobanNetworkConfig> sorobanConfig);

    // Factory: constructs a ImmutableLedgerData, auto-loading the
    // SorobanNetworkConfig from the bucket list when the protocol requires it.
    static ImmutableLedgerDataPtr createAndMaybeLoadConfig(
        LiveBucketList const& liveBL, HotArchiveBucketList const& hotArchiveBL,
        LedgerHeaderHistoryEntry const& lcl, HistoryArchiveState const& has,
        MetricsRegistry& metrics);

    SorobanNetworkConfig const& getSorobanConfig() const;
    bool hasSorobanConfig() const;
    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const;
    HistoryArchiveState const& getLastClosedHistoryArchiveState() const;
};

}
