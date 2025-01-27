// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketSnapshotManager.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTxn.h"
#include "util/NonCopyable.h"
#include <variant>

namespace stellar
{

class Application;
class TransactionFrame;
class LedgerSnapshot;

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
    std::variant<LedgerTxnHeader, std::shared_ptr<LedgerHeader>> mHeader;
    friend class BucketSnapshotState;

  public:
    explicit LedgerHeaderWrapper(LedgerTxnHeader&& header);
    explicit LedgerHeaderWrapper(std::shared_ptr<LedgerHeader> header);
    LedgerHeader& currentToModify();
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
class AbstractLedgerStateSnapshot
{
  public:
    virtual ~AbstractLedgerStateSnapshot() = default;
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
        std::function<void(LedgerSnapshot const&)> f) const = 0;
};

// A concrete implementation of read-only SQL snapshot wrapper
class LedgerTxnReadOnly : public AbstractLedgerStateSnapshot
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
        std::function<void(LedgerSnapshot const&)> f) const override;
};

// A concrete implementation of read-only BucketList snapshot wrapper
class BucketSnapshotState : public AbstractLedgerStateSnapshot
{
    SearchableSnapshotConstPtr const mSnapshot;
    // Store a copy of the header from mSnapshot. This is needed for
    // validation flow where for certain validation scenarios the header needs
    // to be modified
    LedgerHeaderWrapper mLedgerHeader;

  public:
    BucketSnapshotState(SearchableSnapshotConstPtr snapshot);
    ~BucketSnapshotState() override;

    LedgerHeaderWrapper getLedgerHeader() const override;
    LedgerEntryWrapper getAccount(AccountID const& account) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx) const override;
    LedgerEntryWrapper getAccount(LedgerHeaderWrapper const& header,
                                  TransactionFrame const& tx,
                                  AccountID const& AccountID) const override;
    LedgerEntryWrapper load(LedgerKey const& key) const override;
    void executeWithMaybeInnerSnapshot(
        std::function<void(LedgerSnapshot const&)> f) const override;
};

// A helper class to create and query read-only snapshots
// Automatically decides whether to create a BucketList (recommended), or SQL
// snapshot (deprecated, but currently supported)
// NOTE: LedgerSnapshot is meant to be short-lived, and should not be persisted
// across _different_ ledgers, as the state under the hood might change. Users
// are expected to construct a new LedgerSnapshot each time they want to query
// ledger state.
class LedgerSnapshot : public NonMovableOrCopyable
{
    std::unique_ptr<AbstractLedgerStateSnapshot const> mGetter;
    std::unique_ptr<LedgerTxn> mLegacyLedgerTxn;

  public:
    LedgerSnapshot(AbstractLedgerTxn& ltx);
    LedgerSnapshot(Application& app);
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
        std::function<void(LedgerSnapshot const&)> f) const;
};

}
