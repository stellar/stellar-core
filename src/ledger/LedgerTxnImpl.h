// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "util/RandomEvictionCache.h"
#include <list>
#ifdef USE_POSTGRES
#include <iomanip>
#include <libpq-fe.h>
#include <limits>
#include <sstream>
#endif

namespace stellar
{

// Precondition: The keys associated with entries are unique and constitute a
// subset of keys
std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(std::unordered_set<LedgerKey> const& keys,
                      std::vector<LedgerEntry> const& entries);

struct AssetPair
{
    Asset buying;
    Asset selling;
};

bool operator==(AssetPair const& lhs, AssetPair const& rhs);

struct AssetPairHash
{
    size_t operator()(AssetPair const& key) const;
};

// A defensive heuristic to ensure prefetching stops if entry cache is filling
// up.
static const double ENTRY_CACHE_FILL_RATIO = 0.5;

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

    virtual std::unique_ptr<AbstractImpl> clone() const = 0;
};

// Helper struct to accumulate common cases that we can sift out of the
// commit stream and perform in bulk (as single SQL statements per-type)
// rather than making each insert/update/delete individually. This uses the
// postgres and sqlite-supported "ON CONFLICT"-style upserts, and uses
// soci's bulk operations where it can (i.e. for sqlite, or potentially
// others), and manually-crafted postgres unnest([array]) calls where it
// can't. This is not great, but it appears to be less work than
// reorganizing the relevant parts of soci.
class BulkLedgerEntryChangeAccumulator
{

    std::vector<EntryIterator> mAccountsToUpsert;
    std::vector<EntryIterator> mAccountsToDelete;
    std::vector<EntryIterator> mAccountDataToUpsert;
    std::vector<EntryIterator> mAccountDataToDelete;
    std::vector<EntryIterator> mOffersToUpsert;
    std::vector<EntryIterator> mOffersToDelete;
    std::vector<EntryIterator> mTrustLinesToUpsert;
    std::vector<EntryIterator> mTrustLinesToDelete;

  public:
    std::vector<EntryIterator>&
    getAccountsToUpsert()
    {
        return mAccountsToUpsert;
    }

    std::vector<EntryIterator>&
    getAccountsToDelete()
    {
        return mAccountsToDelete;
    }

    std::vector<EntryIterator>&
    getTrustLinesToUpsert()
    {
        return mTrustLinesToUpsert;
    }

    std::vector<EntryIterator>&
    getTrustLinesToDelete()
    {
        return mTrustLinesToDelete;
    }

    std::vector<EntryIterator>&
    getOffersToUpsert()
    {
        return mOffersToUpsert;
    }

    std::vector<EntryIterator>&
    getOffersToDelete()
    {
        return mOffersToDelete;
    }

    std::vector<EntryIterator>&
    getAccountDataToUpsert()
    {
        return mAccountDataToUpsert;
    }

    std::vector<EntryIterator>&
    getAccountDataToDelete()
    {
        return mAccountDataToDelete;
    }

    void accumulate(EntryIterator const& iter);
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
    LedgerTxnConsistency mConsistency;

    void throwIfChild() const;
    void throwIfSealed() const;
    void throwIfNotExactConsistency() const;

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
    void commitChild(EntryIterator iter, LedgerTxnConsistency cons);

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

    // getAllEntries has the strong exception safety guarantee
    void getAllEntries(std::vector<LedgerEntry>& initEntries,
                       std::vector<LedgerEntry>& liveEntries,
                       std::vector<LedgerKey>& deadEntries);

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

    // createOrUpdateWithoutLoading has the strong exception safety guarantee.
    // If it throws an exception, then the current LedgerTxn::Impl is unchanged.
    void createOrUpdateWithoutLoading(LedgerTxn& self,
                                      LedgerEntry const& entry);

    // eraseWithoutLoading has the strong exception safety guarantee. If it
    // throws an exception, then the current LedgerTxn::Impl is unchanged.
    void eraseWithoutLoading(LedgerKey const& key);

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

    std::unique_ptr<EntryIterator::AbstractImpl> clone() const override;
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
    enum class LoadType
    {
        IMMEDIATE,
        PREFETCH
    };

    struct CacheEntry
    {
        std::shared_ptr<LedgerEntry const> entry;
        LoadType type;
    };

    typedef RandomEvictionCache<LedgerKey, CacheEntry> EntryCache;

    typedef AssetPair BestOffersCacheKey;

    struct BestOffersCacheEntry
    {
        std::list<LedgerEntry> bestOffers;
        bool allLoaded;
    };
    typedef std::shared_ptr<BestOffersCacheEntry> BestOffersCacheEntryPtr;

    typedef RandomEvictionCache<BestOffersCacheKey, BestOffersCacheEntryPtr,
                                AssetPairHash>
        BestOffersCache;

    Database& mDatabase;
    std::unique_ptr<LedgerHeader> mHeader;
    mutable EntryCache mEntryCache;
    mutable BestOffersCache mBestOffersCache;
    mutable uint64_t mTotalPrefetchHits{0};

    size_t mMaxCacheSize;
    size_t mBulkLoadBatchSize;
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
    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;
    std::shared_ptr<LedgerEntry const>
    loadTrustLine(LedgerKey const& key) const;

    void bulkApply(BulkLedgerEntryChangeAccumulator& bleca,
                   size_t bufferThreshold, LedgerTxnConsistency cons);
    void bulkUpsertAccounts(std::vector<EntryIterator> const& entries);
    void bulkDeleteAccounts(std::vector<EntryIterator> const& entries,
                            LedgerTxnConsistency cons);
    void bulkUpsertTrustLines(std::vector<EntryIterator> const& entries);
    void bulkDeleteTrustLines(std::vector<EntryIterator> const& entries,
                              LedgerTxnConsistency cons);
    void bulkUpsertOffers(std::vector<EntryIterator> const& entries);
    void bulkDeleteOffers(std::vector<EntryIterator> const& entries,
                          LedgerTxnConsistency cons);
    void bulkUpsertAccountData(std::vector<EntryIterator> const& entries);
    void bulkDeleteAccountData(std::vector<EntryIterator> const& entries,
                               LedgerTxnConsistency cons);

    static std::string tableFromLedgerEntryType(LedgerEntryType let);

    // The entry cache maintains relatively strong invariants:
    //
    //  - It is only ever populated during a database operation, at root.
    //
    //  - Until the (bulk) LedgerTxnRoot::commitChild operation, the only
    //    database operations are SELECTs, which only populate the cache
    //    with fresh data from the DB.
    //
    //  - On LedgerTxnRoot::commitChild, the cache is cleared.
    //
    //  - It is therefore always kept in exact correspondence with the
    //    database for the keyset that it has entries for. It's a precise
    //    image of a subset of the database.
    std::shared_ptr<LedgerEntry const>
    getFromEntryCache(LedgerKey const& key) const;
    void putInEntryCache(LedgerKey const& key,
                         std::shared_ptr<LedgerEntry const> const& entry,
                         LoadType type) const;

    BestOffersCacheEntryPtr getFromBestOffersCache(Asset const& buying,
                                                   Asset const& selling) const;

    std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadAccounts(std::unordered_set<LedgerKey> const& keys) const;
    std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadTrustLines(std::unordered_set<LedgerKey> const& keys) const;
    std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadOffers(std::unordered_set<LedgerKey> const& keys) const;
    std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadData(std::unordered_set<LedgerKey> const& keys) const;

  public:
    // Constructor has the strong exception safety guarantee
    Impl(Database& db, size_t entryCacheSize, size_t bestOfferCacheSize,
         size_t prefetchBatchSize);

    ~Impl();

    // addChild has the strong exception safety guarantee.
    void addChild(AbstractLedgerTxn& child);

    // commitChild has the strong exception safety guarantee.
    void commitChild(EntryIterator iter, LedgerTxnConsistency cons);

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

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    void resetForFuzzer();
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

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

    // Prefetch some or all of given keys in batches. Note that no prefetching
    // could occur if the cache is at its fill ratio. Returns number of keys
    // prefetched.
    uint32_t prefetch(std::unordered_set<LedgerKey> const& keys);

    double getPrefetchHitRate() const;
};

#ifdef USE_POSTGRES
template <typename T>
inline void
marshalToPGArrayItem(PGconn* conn, std::ostringstream& oss, const T& item)
{
    // NB: This setprecision is very important to ensuring that a double
    // gets marshaled to enough decimal digits to reconstruct exactly the
    // same double on the postgres side (that precision-level is exactly
    // what max_digits10 is defined as). Do not remove it!
    oss << std::setprecision(std::numeric_limits<T>::max_digits10) << item;
}

template <>
inline void
marshalToPGArrayItem<std::string>(PGconn* conn, std::ostringstream& oss,
                                  const std::string& item)
{
    std::vector<char> buf(item.size() * 2 + 1, '\0');
    int err = 0;
    size_t len =
        PQescapeStringConn(conn, buf.data(), item.c_str(), item.size(), &err);
    if (err != 0)
    {
        throw std::runtime_error("Could not escape string in SQL");
    }
    oss << '"';
    oss.write(buf.data(), len);
    oss << '"';
}

template <typename T>
inline void
marshalToPGArray(PGconn* conn, std::string& out, const std::vector<T>& v,
                 const std::vector<soci::indicator>* ind = nullptr)
{
    std::ostringstream oss;
    oss << '{';
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i > 0)
        {
            oss << ',';
        }
        if (ind && (*ind)[i] == soci::i_null)
        {
            oss << "NULL";
        }
        else
        {
            marshalToPGArrayItem(conn, oss, v[i]);
        }
    }
    oss << '}';
    out = oss.str();
}
#endif
}
