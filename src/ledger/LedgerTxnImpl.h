// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "util/lrucache.hpp"

namespace stellar
{

class EntryIterator::AbstractImpl
{
  public:
    virtual ~AbstractImpl()
    {
    }

    virtual void advance() = 0;

    virtual bool atEnd() const = 0;

    virtual LedgerEntry const& entry() const = 0;

    virtual bool entryExists() const = 0;

    virtual LedgerKey const& key() const = 0;
};

// Many functions in LedgerTxn::Impl provide a basic exception safety
// guarantee that states that certain caches may be modified or cleared if an
// exception is thrown. It is always safe to continue using the LedgerTxn
// object in such a case and the results of any successful query are correct.
// However, it should be noted that a query which would have succeeded had there
// not been an earlier exception may fail in the case where there had been an
// earlier exception. This could occur, for example, if in the first case the
// query would have hit the cache but in the second case the query hits the
// database because the cache has been cleared but the database connection has
// been lost.
class LedgerTxn::Impl
{
    class EntryIteratorImpl;

    typedef std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry>>
        EntryMap;

    AbstractLedgerTxnParent& mParent;
    AbstractLedgerTxn* mChild;
    std::unique_ptr<LedgerHeader> mHeader;
    std::shared_ptr<LedgerTxnHeader::Impl> mActiveHeader;
    EntryMap mEntry;
    std::unordered_map<LedgerKey, std::shared_ptr<EntryImplBase>> mActive;
    bool const mShouldUpdateLastModified;
    bool mIsSealed;

    void throwIfChild() const;
    void throwIfSealed() const;

    // getDeltaVotes has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    std::map<AccountID, int64_t> getDeltaVotes() const;

    // getTotalVotes has the strong exception safety guarantee
    std::map<AccountID, int64_t>
    getTotalVotes(std::vector<InflationWinner> const& parentWinners,
                  std::map<AccountID, int64_t> const& deltaVotes,
                  int64_t minVotes) const;

    // enumerateInflationWinners has the strong exception safety guarantee
    std::vector<InflationWinner>
    enumerateInflationWinners(std::map<AccountID, int64_t> const& totalVotes,
                              size_t maxWinners, int64_t minVotes) const;

    // getEntryIterator has the strong exception safety guarantee
    EntryIterator getEntryIterator(EntryMap const& entries) const;

    // maybeUpdateLastModified has the strong exception safety guarantee
    EntryMap maybeUpdateLastModified() const;

    // maybeUpdateLastModifiedThenInvokeThenSeal has the same exception safety
    // guarantee as f
    void maybeUpdateLastModifiedThenInvokeThenSeal(
        std::function<void(EntryMap const&)> f);

  public:
    // Constructor has the strong exception safety guarantee
    Impl(LedgerTxn& self, AbstractLedgerTxnParent& parent,
         bool shouldUpdateLastModified);

    // addChild has the strong exception safety guarantee
    void addChild(AbstractLedgerTxn& child);

    // commit has the strong exception safety guarantee.
    void commit();

    // commitChild has the strong exception safety guarantee.
    void commitChild(EntryIterator iter);

    // create has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    LedgerTxnEntry create(LedgerTxn& self, LedgerEntry const& entry);

    // deactivate has the strong exception safety guarantee
    void deactivate(LedgerKey const& key);

    // deactivateHeader has the strong exception safety guarantee
    void deactivateHeader();

    // erase has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    void erase(LedgerKey const& key);

    // getAllOffers has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified.
    std::unordered_map<LedgerKey, LedgerEntry> getAllOffers();

    // getBestOffer has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, modified or even
    //   cleared
    // - the best offers cache may be, but is not guaranteed to be, modified or
    //   even cleared
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::unordered_set<LedgerKey>& exclude);

    // getChanges has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    LedgerEntryChanges getChanges();

    // getDeadEntries has the strong exception safety guarantee
    std::vector<LedgerKey> getDeadEntries();

    // getDelta has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    LedgerTxnDelta getDelta();

    // getOffersByAccountAndAsset has the basic exception safety guarantee. If
    // it throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    std::unordered_map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account, Asset const& asset);

    // getHeader does not throw
    LedgerHeader const& getHeader() const;

    // getInflationWinners has the basic exception safety guarantee. If it
    // throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    std::vector<InflationWinner> getInflationWinners(size_t maxWinners,
                                                     int64_t minBalance);

    // queryInflationWinners has the basic exception safety guarantee. If it
    // throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    std::vector<InflationWinner> queryInflationWinners(size_t maxWinners,
                                                       int64_t minBalance);

    // getLiveEntries has the strong exception safety guarantee
    std::vector<LedgerEntry> getLiveEntries();

    // getNewestVersion has the basic exception safety guarantee. If it throws
    // an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    std::shared_ptr<LedgerEntry const>
    getNewestVersion(LedgerKey const& key) const;

    // load has the basic exception safety guarantee. If it throws an exception,
    // then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    LedgerTxnEntry load(LedgerTxn& self, LedgerKey const& key);

    // loadAllOffers has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    std::map<AccountID, std::vector<LedgerTxnEntry>>
    loadAllOffers(LedgerTxn& self);

    // loadBestOffer has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, modified or even
    //   cleared
    // - the best offers cache may be, but is not guaranteed to be, modified or
    //   even cleared
    LedgerTxnEntry loadBestOffer(LedgerTxn& self, Asset const& buying,
                                 Asset const& selling);

    // loadHeader has the strong exception safety guarantee
    LedgerTxnHeader loadHeader(LedgerTxn& self);

    // loadOffersByAccountAndAsset has the basic exception safety guarantee. If
    // it throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    std::vector<LedgerTxnEntry>
    loadOffersByAccountAndAsset(LedgerTxn& self, AccountID const& accountID,
                                Asset const& asset);

    // loadWithoutRecord has the basic exception safety guarantee. If it throws
    // an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    ConstLedgerTxnEntry loadWithoutRecord(LedgerTxn& self,
                                          LedgerKey const& key);

    // rollback does not throw
    void rollback();

    // rollbackChild does not throw
    void rollbackChild();

    // unsealHeader has the same exception safety guarantee as f
    void unsealHeader(LedgerTxn& self, std::function<void(LedgerHeader&)> f);
};

class LedgerTxn::Impl::EntryIteratorImpl : public EntryIterator::AbstractImpl
{
    typedef LedgerTxn::Impl::EntryMap::const_iterator IteratorType;
    IteratorType mIter;
    IteratorType const mEnd;

  public:
    EntryIteratorImpl(IteratorType const& begin, IteratorType const& end);

    void advance() override;

    bool atEnd() const override;

    LedgerEntry const& entry() const override;

    bool entryExists() const override;

    LedgerKey const& key() const override;
};

// Many functions in LedgerTxnRoot::Impl provide a basic exception safety
// guarantee that states that certain caches may be modified or cleared if an
// exception is thrown. It is always safe to continue using the LedgerTxn
// object in such a case and the results of any successful query are correct.
// However, it should be noted that a query which would have succeeded had there
// not been an earlier exception may fail in the case where there had been an
// earlier exception. This could occur, for example, if in the first case the
// query would have hit the cache but in the second case the query hits the
// database because the cache has been cleared but the database connection has
// been lost.
class LedgerTxnRoot::Impl
{
    typedef std::string EntryCacheKey;
    typedef cache::lru_cache<EntryCacheKey, std::shared_ptr<LedgerEntry const>>
        EntryCache;

    typedef std::string BestOffersCacheKey;
    struct BestOffersCacheEntry
    {
        std::list<LedgerEntry> bestOffers;
        bool allLoaded;
    };
    typedef cache::lru_cache<std::string, BestOffersCacheEntry> BestOffersCache;

    Database& mDatabase;
    std::unique_ptr<LedgerHeader> mHeader;
    mutable EntryCache mEntryCache;
    mutable BestOffersCache mBestOffersCache;
    std::unique_ptr<soci::transaction> mTransaction;
    AbstractLedgerTxn* mChild;

    void throwIfChild() const;

    std::shared_ptr<LedgerEntry const> loadAccount(LedgerKey const& key) const;
    std::shared_ptr<LedgerEntry const> loadData(LedgerKey const& key) const;
    std::shared_ptr<LedgerEntry const> loadOffer(LedgerKey const& key) const;
    std::vector<LedgerEntry> loadAllOffers() const;
    std::list<LedgerEntry>::const_iterator
    loadOffers(StatementContext& prep, std::list<LedgerEntry>& offers) const;
    std::list<LedgerEntry>::const_iterator
    loadBestOffers(std::list<LedgerEntry>& offers, Asset const& buying,
                   Asset const& selling, size_t numOffers, size_t offset) const;
    std::vector<LedgerEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) const;
    std::vector<LedgerEntry> loadOffers(StatementContext& prep) const;
    std::vector<Signer> loadSigners(LedgerKey const& key) const;
    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;
    std::shared_ptr<LedgerEntry const>
    loadTrustLine(LedgerKey const& key) const;

    void storeAccount(EntryIterator const& iter);
    void storeData(EntryIterator const& iter);
    void storeOffer(EntryIterator const& iter);
    void storeTrustLine(EntryIterator const& iter);

    void storeSigners(LedgerEntry const& entry,
                      std::shared_ptr<LedgerEntry const> const& previous);

    void deleteAccount(LedgerKey const& key);
    void deleteData(LedgerKey const& key);
    void deleteOffer(LedgerKey const& key);
    void deleteTrustLine(LedgerKey const& key);

    void insertOrUpdateAccount(LedgerEntry const& entry, bool isInsert);
    void insertOrUpdateData(LedgerEntry const& entry, bool isInsert);
    void insertOrUpdateOffer(LedgerEntry const& entry, bool isInsert);
    void insertOrUpdateTrustLine(LedgerEntry const& entry, bool isInsert);

    static std::string tableFromLedgerEntryType(LedgerEntryType let);

    EntryCacheKey getEntryCacheKey(LedgerKey const& key) const;
    std::shared_ptr<LedgerEntry const>
    getFromEntryCache(EntryCacheKey const& key) const;
    void putInEntryCache(EntryCacheKey const& key,
                         std::shared_ptr<LedgerEntry const> const& entry) const;

    BestOffersCacheEntry&
    getFromBestOffersCache(Asset const& buying, Asset const& selling,
                           BestOffersCacheEntry& defaultValue) const;

  public:
    // Constructor has the strong exception safety guarantee
    Impl(Database& db, size_t entryCacheSize, size_t bestOfferCacheSize);

    ~Impl();

    // addChild has the strong exception safety guarantee.
    void addChild(AbstractLedgerTxn& child);

    // commitChild has the strong exception safety guarantee.
    void commitChild(EntryIterator iter);

    // countObjects has the strong exception safety guarantee.
    uint64_t countObjects(LedgerEntryType let) const;
    uint64_t countObjects(LedgerEntryType let,
                          LedgerRange const& ledgers) const;

    // deleteObjectsModifiedOnOrAfterLedger has no exception safety guarantees.
    void deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const;

    // dropAccounts, dropData, dropOffers, and dropTrustLines have no exception
    // safety guarantees.
    void dropAccounts();
    void dropData();
    void dropOffers();
    void dropTrustLines();

    // getAllOffers has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified.
    std::unordered_map<LedgerKey, LedgerEntry> getAllOffers();

    // getBestOffer has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, modified or even
    //   cleared
    // - the best offers cache may be, but is not guaranteed to be, modified or
    //   even cleared
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::unordered_set<LedgerKey>& exclude);

    // getOffersByAccountAndAsset has the basic exception safety guarantee. If
    // it throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    std::unordered_map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account, Asset const& asset);

    // getHeader does not throw
    LedgerHeader const& getHeader() const;

    // getInflationWinners has the basic exception safety guarantee. If it
    // throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    std::vector<InflationWinner> getInflationWinners(size_t maxWinners,
                                                     int64_t minBalance);

    // getNewestVersion has the basic exception safety guarantee. If it throws
    // an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    std::shared_ptr<LedgerEntry const>
    getNewestVersion(LedgerKey const& key) const;

    // rollbackChild has the strong exception safety guarantee.
    void rollbackChild();
};
}
