#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include <map>
#include <memory>

namespace stellar
{
struct LedgerEntry;
struct LedgerKey;

class ApplicationImpl;

class LedgerEntryReference;
class LedgerHeaderReference;
class LedgerState;

struct InflationVotes
{
    int64 votes;
    AccountID inflationDest;
};

bool operator==(InflationVotes const& lhs, InflationVotes const& rhs);

class LedgerStateRoot
{
    friend class ApplicationImpl;
    friend class LedgerState;

    typedef cache::lru_cache<std::string, std::shared_ptr<LedgerEntry const>>
        EntryCache;

    bool mHasChild;
    Database& mDb;
    EntryCache mEntryCache;
    LedgerHeader mCurrentHeader;

  public:
    LedgerStateRoot(LedgerStateRoot const&) = delete;
    LedgerStateRoot& operator=(LedgerStateRoot const&) = delete;

    LedgerStateRoot(LedgerStateRoot&&) = delete;
    LedgerStateRoot& operator=(LedgerStateRoot&&) = delete;

    void flushCache();

  private:
    explicit LedgerStateRoot(Database& db);

    bool hasChild();
    void hasChild(bool child);

    LedgerHeader const& getCurrentHeader();
    void setCurrentHeader(LedgerHeader const& header);

    Database& getDatabase();
    EntryCache& getCache();
};

class LedgerState
{
    typedef std::shared_ptr<LedgerEntryReference> StateEntry;
    typedef std::shared_ptr<LedgerHeaderReference> StateHeader;

    class LoadBestOfferContext;

    std::unique_ptr<soci::transaction> mTransaction;

    LedgerStateRoot* mRoot;
    LedgerState* mParent;
    LedgerState* mChild;

    // TODO(jonjove): This field is used intensively so it is worth checking
    // whether unordered_map performs better under typical use cases
    std::map<LedgerKey, StateEntry> mState;
    StateHeader mHeader;

    std::shared_ptr<LoadBestOfferContext> mLoadBestOfferContext;

  public:
    explicit LedgerState(LedgerStateRoot& root);
    explicit LedgerState(LedgerState& parent);

    ~LedgerState();

    void commit(std::function<void()> onCommitToDatabase = {});
    void rollback();

    LedgerEntryChanges getChanges();
    std::vector<LedgerEntry> getLiveEntries();
    std::vector<LedgerKey> getDeadEntries();

    void updateLastModified();

    StateEntry create(LedgerEntry const& entry);
    StateEntry load(LedgerKey const& key);
    StateHeader loadHeader();

    StateEntry loadBestOffer(Asset const& selling, Asset const& buying);

    std::vector<InflationVotes> loadInflationWinners(size_t maxWinners,
                                                     int64_t minBalance);

    void forget(StateEntry se);

    class Iterator;

    class IteratorValueType
    {
        friend class LedgerState::Iterator;

      private:
        std::map<LedgerKey, StateEntry>::const_iterator mIter;

      public:
        LedgerKey const& key() const;
        std::shared_ptr<LedgerEntry const> entry() const;
        std::shared_ptr<LedgerEntry const> previousEntry() const;
    };

    class Iterator : public std::iterator<std::input_iterator_tag, IteratorValueType>
    {
        IteratorValueType mValue;

      public:
        Iterator(std::map<LedgerKey, StateEntry>::const_iterator const& iter);

        IteratorValueType const& operator*() const;
        IteratorValueType const* operator->() const;

        Iterator& operator++();

        bool operator==(Iterator const& other) const;
        bool operator!=(Iterator const& other) const;
    };

    Iterator begin() const;
    Iterator end() const;

  private:
    void throwIfAlreadyHandled();
    void throwIfHasChild();
    void throwIfNotRoot();

    StateEntry
    makeStateEntry(std::shared_ptr<LedgerEntry const> const& entry,
                   std::shared_ptr<LedgerEntry const> const& previous);
    StateHeader makeStateHeader(LedgerHeader const& header,
                                LedgerHeader const& previous);

    void mergeStateIntoParent();
    void mergeHeaderIntoParent();
    void mergeStateIntoRoot();
    void mergeHeaderIntoRoot();

    StateEntry createHelper(LedgerEntry const& entry, LedgerKey const& key);
    StateEntry loadHelper(LedgerKey const& key);
    StateHeader loadHeaderHelper();

    StateEntry loadFromDatabase(LedgerKey const& key);
    StateEntry loadAccountFromDatabase(LedgerKey const& key);
    StateEntry loadOfferFromDatabase(LedgerKey const& key);
    StateEntry loadTrustLineFromDatabase(LedgerKey const& key);
    StateEntry loadDataFromDatabase(LedgerKey const& key);

    std::vector<StateEntry> loadOffersFromDatabase(StatementContext& prep);

    void storeInDatabase(StateEntry const& state);
    void storeAccountInDatabase(StateEntry const& state);
    void storeOfferInDatabase(StateEntry const& state);
    void storeTrustLineInDatabase(StateEntry const& state);
    void storeDataInDatabase(StateEntry const& state);

    void storeHeaderInDatabase();

    void deleteFromDatabase(StateEntry const& state);
    void deleteAccountFromDatabase(StateEntry const& state);
    void deleteOfferFromDatabase(StateEntry const& state);
    void deleteTrustLineFromDatabase(StateEntry const& state);
    void deleteDataFromDatabase(StateEntry const& state);

    std::vector<Signer> loadSignersFromDatabase(LedgerKey const& key);
    void storeSignersInDatabase(StateEntry const& state);

    std::vector<StateEntry> loadBestOffersFromDatabase(size_t numOffers,
                                                       size_t offset,
                                                       Asset const& selling,
                                                       Asset const& buying);

    std::shared_ptr<LoadBestOfferContext>
    getLoadBestOfferContext(Asset const& selling, Asset const& buying);

    void invalidateLoadBestOfferContext(Asset const& selling,
                                        Asset const& buying);

    std::vector<LedgerState::StateEntry> getOffers(Asset const& selling,
                                                   Asset const& buying);

    void getOffers(Asset const& selling, Asset const& buying,
                   std::vector<StateEntry>& offers, std::set<LedgerKey>& seen);

    std::vector<InflationVotes>
    loadInflationWinnersFromDatabase(size_t maxWinners, int64_t minBalance);

    bool isInMemory(LedgerKey const& key);
};

class LedgerState::LoadBestOfferContext
{
    typedef std::priority_queue<StateEntry, std::vector<StateEntry>,
                                std::function<bool(StateEntry, StateEntry)>>
        HeapType;

    Asset const mSelling;
    Asset const mBuying;

    LedgerState& mLedgerState;

    StateEntry mTop;
    HeapType mInMemory;
    std::vector<StateEntry> mFromDatabase;
    size_t mOffersLoadedFromDatabase;

    void loadFromDatabaseIfNecessary();

  public:
    explicit LoadBestOfferContext(Asset const& selling, Asset const& buying,
                                  LedgerState& ledgerState);
    explicit LoadBestOfferContext(LoadBestOfferContext&& lboc,
                                  LedgerState& ledgerState);

    LoadBestOfferContext(LoadBestOfferContext const&) = delete;
    LoadBestOfferContext& operator=(LedgerStateRoot const&) = delete;

    LoadBestOfferContext(LoadBestOfferContext&&) = delete;
    LoadBestOfferContext operator=(LoadBestOfferContext&&) = delete;

    Asset const& sellingAsset();
    Asset const& buyingAsset();

    StateEntry loadBestOffer();

    static bool compareOffers(StateEntry const& lhsState,
                              StateEntry const& rhsState);
};

// static helper for getting a LedgerKey from a LedgerEntry.
LedgerKey LedgerEntryKey(LedgerEntry const& e);
}
