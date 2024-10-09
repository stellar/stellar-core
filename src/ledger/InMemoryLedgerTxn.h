#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "ledger/InMemoryLedgerTxnRoot.h"
#include "ledger/LedgerTxn.h"

// This is a (very small) extension of LedgerTxn to help implement in-memory
// mode. In-memory mode only holds the _ledger_ contents in memory; it still has
// a "small" SQL database storing some additional tables, and we still want to
// have transactional atomicity on those tables in regions of code we have a
// LedgerTxn open. So that's the _purpose_.
//
// On to messy implementation details: in-memory mode is implemented by
// replacing the normal LedgerTxnRoot with a stub class InMemoryLedgerTxnRoot
// that never issues _any_ SQL, and then substituting a subclass of LedgerTxn as
// a fake root that stores LEs in memory (like any other LedgerTxn) but that we
// never commit to its parent at all -- only commit children _to_. This class is
// that subclass of LedgerTxn used as a fake root.
//
// Put diagrammatically:
//
//        "Normal" (DB-backed) ledger         "In-memory" ledger
//      --------------------------------+----------------------------------
//               LedgerTxnRoot          |     InMemoryLedgerTxnRoot
//           has soci::transaction      |   has no soci::transaction
//                                      |
//               LedgerTxn              |       InMemoryLedgerTxn
//        has no soci::transaction      |      has soci::transaction
//
//
// In other words, in-memory mode _moves_ the soci::transaction from the root
// to its first (never-closing) child, and commits to the DB when children
// of that first never-closing child commit to it.

namespace stellar
{

class InMemoryLedgerTxn : public LedgerTxn
{
    Database& mDb;
    std::unique_ptr<soci::transaction> mTransaction;

    // For some tests, we need to bypass ledger close and commit directly to the
    // in-memory ltx. However, we still want to test SQL backed offers. The
    // "never" committing root sets this flag to true such that offer-related
    // calls get based to the real SQL backed root
    AbstractLedgerTxnParent* const mRealRootForOffers;

    UnorderedMap<AccountID, UnorderedSet<InternalLedgerKey>>
        mOffersAndPoolShareTrustlineKeys;

    void updateLedgerKeyMap(InternalLedgerKey const& genKey, bool add) noexcept;
    void updateLedgerKeyMap(EntryIterator iter);

    class FilteredEntryIteratorImpl : public EntryIterator::AbstractImpl
    {
        EntryIterator mIter;

      public:
        explicit FilteredEntryIteratorImpl(EntryIterator const& begin);

        void advance() override;

        bool atEnd() const override;

        InternalLedgerEntry const& entry() const override;

        LedgerEntryPtr const& entryPtr() const override;

        bool entryExists() const override;

        InternalLedgerKey const& key() const override;

        std::unique_ptr<EntryIterator::AbstractImpl> clone() const override;
    };

    EntryIterator getFilteredEntryIterator(EntryIterator const& iter);

  public:
    InMemoryLedgerTxn(InMemoryLedgerTxnRoot& parent, Database& db,
                      AbstractLedgerTxnParent* realRoot = nullptr);
    virtual ~InMemoryLedgerTxn();

    void addChild(AbstractLedgerTxn& child, TransactionMode mode) override;
    void commitChild(EntryIterator iter,
                     LedgerTxnConsistency cons) noexcept override;
    void rollbackChild() noexcept override;

    void createWithoutLoading(InternalLedgerEntry const& entry) override;
    void updateWithoutLoading(InternalLedgerEntry const& entry) override;
    void eraseWithoutLoading(InternalLedgerKey const& key) override;

    LedgerTxnEntry create(InternalLedgerEntry const& entry) override;
    void erase(InternalLedgerKey const& key) override;
    LedgerTxnEntry load(InternalLedgerKey const& key) override;
    ConstLedgerTxnEntry
    loadWithoutRecord(InternalLedgerKey const& key) override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                            Asset const& asset) override;

    // These functions call into the real LedgerTxn root to test offer SQL
    // related functionality
    UnorderedMap<LedgerKey, LedgerEntry> getAllOffers() override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling) override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan) override;

    void dropOffers(bool rebuild) override;
    uint64_t countOffers(LedgerRange const& ledgers) const override;
    void deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const override;
};

}
