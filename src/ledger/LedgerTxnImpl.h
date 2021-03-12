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
UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(UnorderedSet<LedgerKey> const& keys,
                      std::vector<LedgerEntry> const& entries);

class EntryIterator::AbstractImpl
{
  public:
    virtual ~AbstractImpl()
    {
    }

    virtual void advance() = 0;

    virtual bool atEnd() const = 0;

    virtual InternalLedgerEntry const& entry() const = 0;

    virtual bool entryExists() const = 0;

    virtual InternalLedgerKey const& key() const = 0;

    virtual std::unique_ptr<AbstractImpl> clone() const = 0;
};

class WorstBestOfferIterator::AbstractImpl
{
  public:
    virtual ~AbstractImpl()
    {
    }

    virtual void advance() = 0;

    virtual AssetPair const& assets() const = 0;

    virtual bool atEnd() const = 0;

    virtual std::shared_ptr<OfferDescriptor const> const&
    offerDescriptor() const = 0;

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
    std::vector<EntryIterator> mClaimableBalanceToUpsert;
    std::vector<EntryIterator> mClaimableBalanceToDelete;
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

    std::vector<EntryIterator>&
    getClaimableBalanceToUpsert()
    {
        return mClaimableBalanceToUpsert;
    }

    std::vector<EntryIterator>&
    getClaimableBalanceToDelete()
    {
        return mClaimableBalanceToDelete;
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
    class WorstBestOfferIteratorImpl;

    typedef UnorderedMap<InternalLedgerKey,
                         std::shared_ptr<InternalLedgerEntry>>
        EntryMap;

    AbstractLedgerTxnParent& mParent;
    AbstractLedgerTxn* mChild;
    std::unique_ptr<LedgerHeader> mHeader;
    std::shared_ptr<LedgerTxnHeader::Impl> mActiveHeader;
    EntryMap mEntry;
    UnorderedMap<InternalLedgerKey, std::shared_ptr<EntryImplBase>> mActive;
    bool const mShouldUpdateLastModified;
    bool mIsSealed;
    LedgerTxnConsistency mConsistency;

    // In theory, we only need an std::map<...> per asset pair. Unfortunately
    // std::map<...> does not provide any remotely exception safe way to update
    // the keys. So we use std::multimap<...> in order to achieve an exception
    // safe update. The observable state of the std::multimap<...> should never
    // have multiple elements with the same key.
    typedef std::multimap<OfferDescriptor, LedgerKey, IsBetterOfferComparator>
        OrderBook;
    typedef UnorderedMap<AssetPair, OrderBook, AssetPairHash> MultiOrderBook;
    // mMultiOrderbook is an in-memory representation of the order book that
    // contains an entry if and only if it is live, and recorded in this
    // LedgerTxn, and not active. It is grouped by asset pair, and for each
    // asset pair all entries are sorted according to the better offer relation.
    //
    // The "if and only if" part of this definition, and the extent to which
    // that makes the mMultiOrderbook _observably exact_ relative to possibly
    // changing entries in mEntry, is maintained by two mechanisms.
    //
    //   - First: the only code that consults mMultiOrderbook (getBestOffer)
    //     checks that mActive is empty, and throws otherwise. So
    //     mMultiOrderbook can't be _observed_ while "edits are in progress"
    //     (entries activated).
    //
    //   - Second: entries are added to mMultiOrderbook when loaded, and then
    //     removed from mMultiOrderbook when made-active (or deleted), and then
    //     added back when deactivated (say after an edit). All this happens via
    //     calls to LedgerTxn::Impl::updateEntry(), which essentially
    //     re-synchronizes an entry in mMultiOrderbook with mEntry/mActive.
    MultiOrderBook mMultiOrderBook;

    // The WorstBestOfferMap is a cache which retains, for each asset pair, the
    // worst value (including possibly nullptr) returned from calling
    // loadBestOffer on this LedgerTxn. Each time we call loadBestOffer, we call
    // updateWorstBestOffer and possibly replace this cached value with the new
    // return value (if it's worse). Then we _use_ this value as a parameter
    // indicating the query restart-point when asking our _parent_ for its
    // next-best offer (in getBestOffer).
    //
    // The WorstBestOfferMap exists to accelerate _repeated_ calls to
    // loadBestOffer within _nested_ LedgerTxns. You could remove it and all
    // associated logic and everything would still _work_, but it would be too
    // slow.
    //
    // Specifically: in the performance-critical loop of convertWithOffers in
    // transactions/OfferExchange.cpp, an "outer" loop-spanning LedgerTxn is
    // held open while sub-LedgerTxns are, inside the loop, repeatedly opened
    // and committed against it, each requesting and then crossing one next-best
    // offer. While this "outer" LedgerTxn's MultiOrderBook will be kept
    // up-to-date with respect to the depleting supply of offers, its _parent_
    // LedgerTxn will answer each such request starting from its own
    // MultiOrderBook, which contains an increasingly-long sequence of offers
    // that have already been crossed and marked dead in the loop-spanning
    // LedgerTxn.
    //
    // The WorstBestOfferMap accelerates this specific case (and cases like it),
    // but it's worth understanding how, very clearly. Here's a diagram,
    // simplified to both collapse the MultiOrderBook and Entry/Active map, and
    // only deal with the OrderBook and WorstBestOffer of a single asset-pair.
    //
    //
    //  +-------------------------------------------------------+
    //  |Transaction-spanning LedgerTxn "X"                     |
    //  |                                                       |
    //  | +-------+  +-------+  +-------+  +-------+  +-------+ |
    //  | |Offer A|  |Offer B|  |Offer C|  |Offer D|  |Offer E| |
    //  | |$0.20  |  |$0.25  |  |$0.30  |  |$0.35  |  |$0.40  | |
    //  | |LIVE   |  |LIVE   |  |LIVE   |  |LIVE   |  |LIVE   | |
    //  | +-------+  +-------+  +-------+  +-------+  +-------+ |
    //  +-------------------------------------------------------+
    //                              ^
    //                              | Parent
    //                              |
    //  +-------------------------------------------------------+
    //  |Operation loop-spanning LedgerTxn "Y"                  |
    //  |                                                       |
    //  | +--------------+                                      |
    //  | |WorstBestOffer|----------+                           |
    //  | +--------------+          v                           |
    //  | +-------+  +-------+  +-------+  +-------+            |
    //  | |Offer A|  |Offer B|  |Offer C|  |Offer D|            |
    //  | |$0.20  |  |$0.25  |  |$0.30  |  |$0.35  |            |
    //  | |DEAD   |  |DEAD   |  |DEAD   |  |LIVE   |            |
    //  | +-------+  +-------+  +-------+  +-------+            |
    //  +-------------------------------------------------------+
    //                              ^
    //                              | Parent
    //                              |
    //  +-------------------------------------------------------+
    //  |Loop-iteration LedgerTxn "Z"                           |
    //  |                                                       |
    //  |   +--------------+                                    |
    //  |   |WorstBestOffer|-------------------+                |
    //  |   +--------------+                   v                |
    //  |                                  +-------+            |
    //  |                                  |Offer D|            |
    //  |                                  |$0.35  |            |
    //  |                                  |LIVE   |            |
    //  |                                  +-------+            |
    //  +-------------------------------------------------------+
    //
    // This diagram shows innermost LedgerTxn Z which has called loadBestOffer
    // and received offer D, which it will then cross, committing a dead entry
    // for that offer (D) into its loop-spanning LedgerTxn parent Y. This
    // already happened for offers A, B and C, and will proceed to E. This
    // is a typical pattern.
    //
    // Note however that this means Y is accumulating a sequence of dead offers
    // in its entry map. They're not in its MultiOrderBook (they do not
    // interfere with finding the next-best offer Y knows about), but they are
    // "pending changes" that Y has accumulated and needs to exclude from
    // consideration any time it asks _its_ parent, X, for a next-best offer.
    //
    // As shown, X has no idea there's a pending set of offers-to-be-deleted in
    // its child Y, so naively (without a WorstBestOffer cache) if, after Z
    // commits its illustrated delete of D to Y, Z's successor-iteration then
    // asks Y for the next-best offer, Y will ask X for its next-best offer, and
    // X will return A, which is correct from X's perspective, but obsolete from
    // Y's perspective. Y would then have to "reject" that returned A and ask
    // again to get B, C, and D, before getting to the "actual" next-best offer
    // (from Y's perspective) E.
    //
    // The WorstBestOfferMap (cache) exists to bypass this re-scanning of
    // entries in X: Z retains a pointer-to-D from the result of its call to
    // loadBestOffers, and when Z commits to Y, Z _also_ commits its
    // WorstBestOffer (D) to Y, which will update Y's WorstBestOffer.  Then when
    // Y asks X for the next-best offer, it'll pass (as a filter) its
    // WorstBestOffer (D) and X will immediately return E, avoiding the re-scan
    // of dead offers.
    //
    // Note in the diagram that the WorstBestOffer value in Y was _not_ set to
    // its value by a call to loadBestOffers in Y. Rather it was set by
    // _commits_ coming in from the inner Z child-LedgerTxns. The
    // WorstBestOfferMap is surprising this way: it's initially "populated" at
    // the innermost child, but then propagated on commit to that child's
    // parent, and finally _used_ when that parent asks _its_ parent to
    // getBestOffer.

    typedef UnorderedMap<AssetPair, std::shared_ptr<OfferDescriptor const>,
                         AssetPairHash>
        WorstBestOfferMap;
    // The exact definition / invariant of the WorstBestOfferMap's data is
    // unfortunately a bit subtle.
    //
    // In what follows, we will only work with offer-descriptors. The defintions
    // are equally valid with any instance of offer-descriptor changed to offer.
    //
    // We say an offer-descriptor A is worse than an offer-descriptor B if
    //
    //     A.price > B.price || (A.price == B.price && A.offerID > B.offerID)
    //
    // We write this as A > B, and write !(A > B) as A <= B to denote that A is
    // not worse than B.
    //
    // We say a pointer-to-offer-descriptor A is worse than a
    // pointer-to-offer-descriptor B if
    //
    //     B && (!A || *A > *B)
    //
    // We again write this as A > B, and write !(A > B) as A <= B to denote that
    // A is not worse than B. That nullptr > &B for any offer-descriptor B is
    // motivated by the fact that nullptr is only the result of loadBestOffer if
    // there are no offers for the specified asset pair.
    //
    // Let LtEq[L, P, B] be the set of all offers O with asset pair P that exist
    // as of the LedgerTxn L and are <= B.
    //
    // If the worst best offer map contains an asset pair P with
    // pointer-to-offer-descriptor V, then every offer in LtEq[Parent, P, V] has
    // been recorded in this LedgerTxn. Note that V is not guaranteed to be the
    // worst pointer-to-offer-descriptor that satisfies this
    // requirement. Informally, it is possible that offers with asset pair P
    // that existed as of the Parent and are worse than V have also been
    // recorded in this LedgerTxn.
    WorstBestOfferMap mWorstBestOffer;

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

    // findInOrderBook has the strong exception safety guarantee
    std::pair<MultiOrderBook::iterator, OrderBook::iterator>
    findInOrderBook(LedgerEntry const& le);

    // updateEntryIfRecorded and updateEntry have the strong exception safety
    // guarantee
    void updateEntryIfRecorded(InternalLedgerKey const& key,
                               bool effectiveActive);
    void updateEntry(InternalLedgerKey const& key,
                     std::shared_ptr<InternalLedgerEntry> lePtr);
    void updateEntry(InternalLedgerKey const& key,
                     std::shared_ptr<InternalLedgerEntry> lePtr,
                     bool effectiveActive);
    void updateEntry(InternalLedgerKey const& key,
                     std::shared_ptr<InternalLedgerEntry> lePtr,
                     bool effectiveActive, bool eraseIfNull);

    // updateWorstBestOffer has the strong exception safety guarantee
    void updateWorstBestOffer(AssetPair const& assets,
                              std::shared_ptr<OfferDescriptor const> offerDesc);

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
    LedgerTxnEntry create(LedgerTxn& self, InternalLedgerEntry const& entry);

    // deactivate has the strong exception safety guarantee
    void deactivate(InternalLedgerKey const& key);

    // deactivateHeader has the strong exception safety guarantee
    void deactivateHeader();

    // erase has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    void erase(InternalLedgerKey const& key);

    // getAllOffers has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified.
    UnorderedMap<LedgerKey, LedgerEntry> getAllOffers();

    // getBestOffer has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, modified or even
    //   cleared
    // - the best offers cache may be, but is not guaranteed to be, modified or
    //   even cleared
    std::shared_ptr<LedgerEntry const> getBestOffer(Asset const& buying,
                                                    Asset const& selling);
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan);

    // getWorstBestOfferIterator has the strong exception safety guarantee
    WorstBestOfferIterator getWorstBestOfferIterator();

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
    UnorderedMap<LedgerKey, LedgerEntry>
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
    std::shared_ptr<InternalLedgerEntry const>
    getNewestVersion(InternalLedgerKey const& key) const;

    // load has the basic exception safety guarantee. If it throws an exception,
    // then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    // - the entry cache may be, but is not guaranteed to be, cleared.
    LedgerTxnEntry load(LedgerTxn& self, InternalLedgerKey const& key);

    // createOrUpdateWithoutLoading has the strong exception safety guarantee.
    // If it throws an exception, then the current LedgerTxn::Impl is unchanged.
    void createOrUpdateWithoutLoading(LedgerTxn& self,
                                      InternalLedgerEntry const& entry);

    // eraseWithoutLoading has the strong exception safety guarantee. If it
    // throws an exception, then the current LedgerTxn::Impl is unchanged.
    void eraseWithoutLoading(InternalLedgerKey const& key);

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
                                          InternalLedgerKey const& key);

    // rollback does not throw
    void rollback();

    // rollbackChild does not throw
    void rollbackChild();

    // unsealHeader has the same exception safety guarantee as f
    void unsealHeader(LedgerTxn& self, std::function<void(LedgerHeader&)> f);

    uint32_t prefetch(UnorderedSet<LedgerKey> const& keys);

    double getPrefetchHitRate() const;

    // hasSponsorshipEntry has the strong exception safety guarantee
    bool hasSponsorshipEntry() const;

#ifdef BUILD_TESTS
    MultiOrderBook const& getOrderBook();
#endif

#ifdef BEST_OFFER_DEBUGGING
    bool bestOfferDebuggingEnabled() const;

    std::shared_ptr<LedgerEntry const>
    getBestOfferSlow(Asset const& buying, Asset const& selling,
                     OfferDescriptor const* worseThan,
                     std::unordered_set<int64_t>& exclude);

    std::shared_ptr<LedgerEntry const>
    checkBestOffer(Asset const& buying, Asset const& selling,
                   OfferDescriptor const* worseThan,
                   std::shared_ptr<LedgerEntry const> best);
#endif
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

    InternalLedgerEntry const& entry() const override;

    bool entryExists() const override;

    InternalLedgerKey const& key() const override;

    std::unique_ptr<EntryIterator::AbstractImpl> clone() const override;
};

class LedgerTxn::Impl::WorstBestOfferIteratorImpl
    : public WorstBestOfferIterator::AbstractImpl
{
    typedef LedgerTxn::Impl::WorstBestOfferMap::const_iterator IteratorType;
    IteratorType mIter;
    IteratorType const mEnd;

  public:
    WorstBestOfferIteratorImpl(IteratorType const& begin,
                               IteratorType const& end);

    AssetPair const& assets() const override;

    void advance() override;

    bool atEnd() const override;

    std::shared_ptr<OfferDescriptor const> const&
    offerDescriptor() const override;

    std::unique_ptr<WorstBestOfferIterator::AbstractImpl>
    clone() const override;
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

    typedef AssetPair BestOffersKey;

    struct BestOffersEntry
    {
        std::deque<LedgerEntry> bestOffers;
        bool allLoaded;
    };
    typedef std::shared_ptr<BestOffersEntry> BestOffersEntryPtr;

    typedef UnorderedMap<BestOffersKey, BestOffersEntryPtr, AssetPairHash>
        BestOffers;

    static size_t const MIN_BEST_OFFERS_BATCH_SIZE;
    size_t const mMaxBestOffersBatchSize;

    Database& mDatabase;
    std::unique_ptr<LedgerHeader> mHeader;
    mutable EntryCache mEntryCache;
    mutable BestOffers mBestOffers;
    mutable uint64_t mPrefetchHits{0};
    mutable uint64_t mPrefetchMisses{0};

    size_t mBulkLoadBatchSize;
    std::unique_ptr<soci::transaction> mTransaction;
    AbstractLedgerTxn* mChild;

#ifdef BEST_OFFER_DEBUGGING
    bool const mBestOfferDebuggingEnabled;
#endif

    void throwIfChild() const;

    std::shared_ptr<LedgerEntry const> loadAccount(LedgerKey const& key) const;
    std::shared_ptr<LedgerEntry const> loadData(LedgerKey const& key) const;
    std::shared_ptr<LedgerEntry const> loadOffer(LedgerKey const& key) const;
    std::vector<LedgerEntry> loadAllOffers() const;
    std::deque<LedgerEntry>::const_iterator
    loadOffers(StatementContext& prep, std::deque<LedgerEntry>& offers) const;
    std::deque<LedgerEntry>::const_iterator
    loadBestOffers(std::deque<LedgerEntry>& offers, Asset const& buying,
                   Asset const& selling, size_t numOffers) const;
    std::deque<LedgerEntry>::const_iterator
    loadBestOffers(std::deque<LedgerEntry>& offers, Asset const& buying,
                   Asset const& selling, OfferDescriptor const& worseThan,
                   size_t numOffers) const;
    std::vector<LedgerEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) const;
    std::vector<LedgerEntry> loadOffers(StatementContext& prep) const;
    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;
    std::shared_ptr<LedgerEntry const>
    loadTrustLine(LedgerKey const& key) const;
    std::shared_ptr<LedgerEntry const>
    loadClaimableBalance(LedgerKey const& key) const;

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
    void bulkUpsertClaimableBalance(std::vector<EntryIterator> const& entries);
    void bulkDeleteClaimableBalance(std::vector<EntryIterator> const& entries,
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
    std::shared_ptr<InternalLedgerEntry const>
    getFromEntryCache(LedgerKey const& key) const;
    void putInEntryCache(LedgerKey const& key,
                         std::shared_ptr<LedgerEntry const> const& entry,
                         LoadType type) const;

    BestOffersEntryPtr getFromBestOffers(Asset const& buying,
                                         Asset const& selling) const;

    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadAccounts(UnorderedSet<LedgerKey> const& keys) const;
    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadTrustLines(UnorderedSet<LedgerKey> const& keys) const;
    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadOffers(UnorderedSet<LedgerKey> const& keys) const;
    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadData(UnorderedSet<LedgerKey> const& keys) const;
    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
    bulkLoadClaimableBalance(UnorderedSet<LedgerKey> const& keys) const;

    std::deque<LedgerEntry>::const_iterator
    loadNextBestOffersIntoCache(BestOffersEntryPtr cached, Asset const& buying,
                                Asset const& selling);
    void populateEntryCacheFromBestOffers(
        std::deque<LedgerEntry>::const_iterator iter,
        std::deque<LedgerEntry>::const_iterator const& end);

    bool areEntriesMissingInCacheForOffer(OfferEntry const& oe);

  public:
    // Constructor has the strong exception safety guarantee
    Impl(Database& db, size_t entryCacheSize, size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
         ,
         bool bestOfferDebuggingEnabled
#endif
    );

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
    void dropClaimableBalances();

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    void resetForFuzzer();
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

    // getAllOffers has the basic exception safety guarantee. If it throws an
    // exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified.
    UnorderedMap<LedgerKey, LedgerEntry> getAllOffers();

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
                 OfferDescriptor const* worseThan);

    // getOffersByAccountAndAsset has the basic exception safety guarantee. If
    // it throws an exception, then
    // - the prepared statement cache may be, but is not guaranteed to be,
    //   modified
    UnorderedMap<LedgerKey, LedgerEntry>
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
    std::shared_ptr<InternalLedgerEntry const>
    getNewestVersion(InternalLedgerKey const& key) const;

    // rollbackChild has the strong exception safety guarantee.
    void rollbackChild();

    // Prefetch some or all of given keys in batches. Note that no prefetching
    // could occur if the cache is at its fill ratio. Returns number of keys
    // prefetched.
    uint32_t prefetch(UnorderedSet<LedgerKey> const& keys);

    double getPrefetchHitRate() const;

#ifdef BEST_OFFER_DEBUGGING
    bool bestOfferDebuggingEnabled() const;

    std::shared_ptr<LedgerEntry const>
    getBestOfferSlow(Asset const& buying, Asset const& selling,
                     OfferDescriptor const* worseThan,
                     std::unordered_set<int64_t>& exclude);

    std::shared_ptr<LedgerEntry const>
    checkBestOffer(Asset const& buying, Asset const& selling,
                   OfferDescriptor const* worseThan,
                   std::shared_ptr<LedgerEntry const> best);
#endif
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
