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
#include <optional>
#include <variant>

namespace stellar
{

class Application;
class TransactionFrame;
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
    // Returns the pointer to Soroban network config snapshot associated with
    // this view, or nullptr when the view doesn't carry one.
    virtual SorobanNetworkConfig const* getSorobanNetworkConfig() const = 0;
    virtual LedgerEntryWrapper getAccount(AccountID const& account) const = 0;
    virtual LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                          TransactionFrame const& tx) const = 0;
    virtual LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                          TransactionFrame const& tx,
                                          AccountID const& AccountID) const = 0;
    virtual LedgerEntryWrapper load(LedgerKey const& key) const = 0;
};

// A read-only view backed by a ledger transaction.
//
// This should only serve as an adapter for the rare cases where LTX has to
// be passed to a function that expects an `AbstractLedgerView`. Prefer using
// other view types, or work with the `LedgerTxn` directly.
class LedgerTxnView : public AbstractLedgerView, public NonMovableOrCopyable
{
    std::optional<SorobanNetworkConfig> mLoadedConfig;
    AbstractLedgerTxn& mLedgerTxn;
    SorobanNetworkConfig const* mSorobanConfig{nullptr};

  public:
    explicit LedgerTxnView(AbstractLedgerTxn& ltx);
    // Takes an externally provided Soroban config (nullptr before the Soroban
    // protocol version) instead of loading it from ltx.
    // This only exists as optimization to avoid re-loading the config when many
    // short-term views are created (e.g. for transaction/operation processing).
    LedgerTxnView(AbstractLedgerTxn& ltx,
                  SorobanNetworkConfig const* sorobanConfig);
    ~LedgerTxnView() override = default;
    LedgerHeaderWrapper getLedgerHeader() const override;
    SorobanNetworkConfig const* getSorobanNetworkConfig() const override;
    LedgerEntryWrapper getAccount(AccountID const& account) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx,
                                  AccountID const& accountID) const override;
    LedgerEntryWrapper load(LedgerKey const& key) const override;
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
    SorobanNetworkConfig const* getSorobanNetworkConfig() const override;
    uint32_t getLedgerSeq() const;

    // === AbstractLedgerView overrides ===
    LedgerEntryWrapper getAccount(AccountID const& account) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx,
                                  AccountID const& AccountID) const override;
    LedgerEntryWrapper load(LedgerKey const& key) const override;

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

    // Scan the live bucket list for entries of a given type. Note this iterates
    // over all BucketEntry, so some may be shadowed and outdated.
    void scanLiveEntriesOfType(
        LedgerEntryType type,
        std::function<Loop(BucketEntry const&)> callback) const;

    // Scan the live bucket list for entries of a given type. Calls callback
    // with the latest live version for each entry.
    void scanCurrentLiveEntriesOfType(
        LedgerEntryType type,
        std::function<void(LedgerEntry const&, LedgerKey const&)> callback)
        const;

    // === Hot Archive BucketList methods ===
    std::shared_ptr<HotArchiveBucketEntry const>
    loadArchiveEntry(LedgerKey const& k) const;
    std::vector<HotArchiveBucketEntry>
    loadArchiveKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                    std::string const& label) const;
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

    using ImmutableLedgerView::getAccount;
    using ImmutableLedgerView::getLedgerHeader;
    using ImmutableLedgerView::getLedgerSeq;
    using ImmutableLedgerView::getSorobanNetworkConfig;
    using ImmutableLedgerView::getState;
    using ImmutableLedgerView::load;
    using ImmutableLedgerView::loadArchiveEntry;
    using ImmutableLedgerView::loadArchiveKeys;
    using ImmutableLedgerView::loadInflationWinners;
    using ImmutableLedgerView::loadLiveEntry;
    using ImmutableLedgerView::loadLiveKeys;
    using ImmutableLedgerView::loadPoolShareTrustLinesByAccountAndAsset;
    using ImmutableLedgerView::scanAllArchiveEntries;
    using ImmutableLedgerView::scanCurrentLiveEntriesOfType;
    using ImmutableLedgerView::scanForEviction;
    using ImmutableLedgerView::scanLiveEntriesOfType;
};

// A ledger view used by the read-only phase of the Soroban pre-apply.
//
// It's a thin wrapper around the entries updated so far in the current ledger,
// and the LCL view, which allows the pre-apply phase to observe the changes
// that happened in the classic phase.
//
// Lookups are first attempted among the updated entries, and only then in the
// LCL view.
class SorobanPreApplyLedgerView : public AbstractLedgerView
{
  public:
    // A function for retrieving a ledger entry by key from an incomprehensive
    // set of ledger entries (i.e. the entries that have been updated so far in
    // the ledger).
    // `nullopt` represents that the entry is not present in the updated set.
    // When optional is non-nullopt, `nullptr` entry represents a deleted entry,
    // and a non-null `shared_ptr` represents an existing updated entry.
    using UpdatedEntryGetter =
        std::function<std::optional<std::shared_ptr<LedgerEntry const>>(
            LedgerKey const&)>;

    // Creates a view from the provided LCL view and a getter for the entries
    // that have been updated so far in the ledger.
    SorobanPreApplyLedgerView(std::shared_ptr<LedgerHeader const> header,
                              UpdatedEntryGetter getUpdatedEntry,
                              ApplyLedgerView const& lclView);

    LedgerHeaderWrapper getLedgerHeader() const override;
    SorobanNetworkConfig const* getSorobanNetworkConfig() const override;
    LedgerEntryWrapper getAccount(AccountID const& account) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx,
                                  AccountID const& accountID) const override;
    LedgerEntryWrapper load(LedgerKey const& key) const override;

  private:
    std::shared_ptr<LedgerHeader const> mHeader;
    UpdatedEntryGetter mGetUpdatedEntry;
    ApplyLedgerView mLclView;
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

    // Pre-resolved metric references shared by all views over this state.
    // Resolving metrics takes the global registry lock, so they are resolved
    // once here instead of in every view construction, which is on the
    // per-transaction hot path.
    std::shared_ptr<BucketSnapshotMetrics<LiveBucket> const> const
        mLiveSnapshotMetrics;
    std::shared_ptr<BucketSnapshotMetrics<HotArchiveBucket> const> const
        mHotArchiveSnapshotMetrics;

    LedgerHeaderHistoryEntry const mLastClosedLedgerHeader;
    HistoryArchiveState const mLastClosedHistoryArchiveState;
    std::optional<SorobanNetworkConfig const> mSorobanConfig;

    void checkInvariant() const;

    friend class ImmutableLedgerView;

  public:
    // Construct a new immutable ledger state snapshot.
    // sorobanConfig may be nullopt, in which case the configuration is loaded
    // from the live bucket list whenever the protocol version supports Soroban.
    ImmutableLedgerData(LiveBucketList const& liveBL,
                        HotArchiveBucketList const& hotArchiveBL,
                        LedgerHeaderHistoryEntry const& lcl,
                        HistoryArchiveState const& has,
                        std::optional<SorobanNetworkConfig> sorobanConfig,
                        MetricsRegistry& metrics);

    SorobanNetworkConfig const& getSorobanConfig() const;
    bool hasSorobanConfig() const;
    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const;
    HistoryArchiveState const& getLastClosedHistoryArchiveState() const;
};

}
