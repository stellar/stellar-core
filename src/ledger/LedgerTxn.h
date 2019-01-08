#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "xdr/Stellar-ledger.h"
#include <functional>
#include <ledger/LedgerHashUtils.h>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

/////////////////////////////////////////////////////////////////////////////
//  Overview
/////////////////////////////////////////////////////////////////////////////
//
// The LedgerTxn subsystem consists of a number of classes (made a bit
// more numerous through the use of inner ::Impl "compiler firewall"
// classes and abstract base classes), of which the essential members and
// relationships are diagrammed here.
//
//
//  +-----------------------------------+
//  |LedgerTxnRoot                      |
//  |(will commit child entries to DB)  |
//  |                                   |
//  |Database &mDatabase                |
//  |AbstractLedgerTxn *mChild -------------+
//  +-----------------------------------+   |
//      ^                                   v
//      |   +-----------------------------------+
//      |   |LedgerTxn                          |
//      |   |(will commit child entries to self)|
//      |   |                                   |
//      +----AbstractLedgerTxnParent &mParent   |
//          |AbstracLedgerTxn *mChild ------------+
//          +-----------------------------------+ |
//                ^                               v
//                |    +-----------------------------------------------------+
//                |    |LedgerTxn : AbstractLedgerTxn                        |
//                |    |(an in-memory transaction-in-progress)               |
//                |    |                                                     |
//                |    |            void commit()                            |
//                |    |            void rollback()                          |
//                |    |LedgerTxnEntry create(LedgerEntry)                   |
//                |    |LedgerTxnEntry load(LedgerKey)                       |
//                |    |            void erase(LedgerKey)                    |
//                |    |                                                     |
//                |    |+---------------------------------------------------+|
//                |    ||LedgerTxn::Impl                                    ||
//                |    ||                                                   ||
//                +------AbstractLedgerTxnParent &mParent                   ||
//                     ||AbstractLedgerTxn *mChild = nullptr                ||
//                     ||                                                   ||
//  +----------------+ ||+------------------------------+ +----------------+||
//  |LedgerTxnEntry  | |||mActive                       | |mEntry          |||
//  |(for client use)| |||                              | |                |||
//  |                | |||map<LedgerKey,                | |map<LedgerKey,  |||
//  |weak_ptr<Impl>  | |||    shared_ptr<EntryImplBase>>| |    LedgerEntry>|||
//  +----------------+ ||+------------------------------+ +----------------+||
//           |         |+---------------------------------------------------+|
//                     +-----------------------------------------------------+
//           |                                          ^
//                       +-------------------------+    |  +-------------+
//           |           |+-------------------------+   |  |+-------------+
//                       ||+-------------------------+  |  ||+-------------+
//           |           |||LedgerTxnEntry::Impl     |  |  +||LedgerEntry  |
//         weak - - - - >|||(indicates "entry is     |  |   +|(XDR object) |
//                       |||active in this state")   |  |    +-------------+
//                       |||                         |  |           ^
//                       +||AbstractLedgerTxn &  -------+           |
//                        +|LedgerEntry &        -------------------+
//                         +-------------------------+
//
//
// The following notes may help with orientation and understanding:
//
//  - A LedgerTxn is an in-memory transaction-in-progress against the
//    ledger in the database. Its ultimate purpose is to model a collection
//    of LedgerEntry (XDR) objects to commit to the database.
//
//  - At any given time, a LedgerTxn may have zero-or-one active
//    sub-transactions, arranged in a parent/child relationship. The terms
//    "parent" and "child" refer exclusively to this nesting-relationship
//    of transactions. The presence of an active sub-LedgerTxn is indicated
//    by a non-null mChild pointer.
//
//  - Once a child is closed and the mChild pointer is reset to null,
//    a new child may be opened. Attempting to open two children at once
//    will throw an exception.
//
//  - The entries to be committed in each transaction are stored in the
//    mEntry map, keyed by LedgerKey. This much is straightforward!
//
//  - Committing any LedgerTxn merges its entries into its parent. In the
//    case where the parent is simply another in-memory LedgerTxn, this
//    means writing the entries into the parent's mEntries map. In the case
//    where the parent is the LedgerTxnRoot, this means opening a Real SQL
//    Transaction against the database and writing the entries to it.
//
//  - Each entry may also be designated as _active_ in a given LedgerTxn;
//    tracking active-ness is the purpose of the other (mActive) map in
//    the diagram above. Active-ness is a logical state that simply means
//    "it is ok, from a concurrency-control perspective, for a client to
//    access this entry in this LedgerTxn." See below for the
//    concurrency-control issues this is designed to trap.
//
//  - Entries are made-active by calling load() or create(), each of which
//    returns a LedgerTxnEntry which is a handle that can be used to get at
//    the underlying LedgerEntry. References to the underlying
//    LedgerEntries should generally not be retained anywhere, because the
//    LedgerTxnEntry handles may be "deactivated", and access to a
//    deactivated entry is a _logic error_ in the client that this
//    machinery is set up to try to trap. If you hold a reference to the
//    underlying entry, you're bypassing the checking machinery that is
//    here to catch such errors. Don't do it.
//
//  - load()ing an entry will either check the current LedgerTxn for an
//    entry, or if none is found it will ask its parent. This process
//    recurses until it hits an entry or terminates at the root, where an
//    LRU cache is consulted and then (finally!) the database itself.
//
//  - The LedgerTxnEntry handles that clients should use are
//    double-indirect references.
//
//      - The first level of indirection is a LedgerTxnEntry::Impl, which
//        is an internal 2-word binding stored in the mActive map that
//        serves simply track the fact that an entry _is_ active, and to
//        facilitate deactivating the entry.
//
//      - The second level of indirection is the client-facing type
//        LedgerTxnEntry, which is _weakly_ linked to its ::Impl type (via
//        std::weak_ptr). This weak linkage enables the LedgerTxn to
//        deactivate entries without worrying that some handle might remain
//        able to access them (assuming they did not hold references to the
//        inner LedgerEntries).
//
//  - The purpose of the double-indirection is to maintain one critical
//    invariant in the system: clients can _only access_ the entries in the
//    innermost (child-most) LedgerTxn open at any given time. This is
//    enforced by deactivating all the entries in a parent LedgerTxn when a
//    child is opened. The entries in the parent still exist in its mEntry
//    map (and will be committed to the parent's parent when the parent
//    commits); but they are not _active_, meaning that attempts to access
//    them through any LedgerTxnEntry handles will throw an exception.
//
//  - The _reason_ for this invariant is to prevent concurrency anomalies:
//
//      - Stale reads: a client could open a sub-transaction, write some
//        entries into it, and then accidentally read from the parent and
//        thereby observe stale data.
//
//      - Lost updates: a client could open a sub-transaction, write some
//        entries to it, and then accidentally write more updates to those
//        same entries to the parent, which would be overwritten by the
//        child when it commits.
//
//    Both these anomalies are harder to cause if the interface refuses all
//    accesses to a parent's entries when a child is open.
//

namespace stellar
{

class Database;
struct InflationVotes;
struct LedgerEntry;
struct LedgerKey;
class LedgerRange;

bool isBetterOffer(LedgerEntry const& lhsEntry, LedgerEntry const& rhsEntry);

class AbstractLedgerTxn;

struct InflationWinner
{
    AccountID accountID;
    int64_t votes;
};

// LedgerTxnDelta represents the difference between a LedgerTxn and its
// parent. Used in the Invariants subsystem.
struct LedgerTxnDelta
{
    struct EntryDelta
    {
        std::shared_ptr<LedgerEntry const> current;
        std::shared_ptr<LedgerEntry const> previous;
    };

    struct HeaderDelta
    {
        LedgerHeader current;
        LedgerHeader previous;
    };

    std::unordered_map<LedgerKey, EntryDelta> entry;
    HeaderDelta header;
};

// An abstraction for an object that is iterator-like and permits enumerating
// the LedgerTxnEntry objects managed by an AbstractLedgerTxn. This enables
// an AbstractLedgerTxnParent to iterate over the entries managed by its child
// without any knowledge of the implementation of the child.
class EntryIterator
{
  public:
    class AbstractImpl;

  private:
    std::unique_ptr<AbstractImpl> mImpl;

    std::unique_ptr<AbstractImpl> const& getImpl() const;

  public:
    EntryIterator(std::unique_ptr<AbstractImpl>&& impl);

    EntryIterator(EntryIterator&& other);

    EntryIterator& operator++();

    explicit operator bool() const;

    LedgerEntry const& entry() const;

    bool entryExists() const;

    LedgerKey const& key() const;
};

// An abstraction for an object that can be the parent of an AbstractLedgerTxn
// (discussed below). Allows children to commit atomically to the parent. Has no
// notion of a LedgerTxnEntry or LedgerTxnHeader (discussed respectively in
// LedgerTxnEntry.h and LedgerTxnHeader.h) but allows access to XDR objects
// such as LedgerEntry and LedgerHeader. This interface is designed such that
// concrete implementations can be databases or AbstractLedgerTxn objects. In
// general, this interface was not designed to be used directly by end users.
// Rather, end users should interact with AbstractLedgerTxnParent through the
// AbstractLedgerTxn interface.
class AbstractLedgerTxnParent
{
  public:
    virtual ~AbstractLedgerTxnParent();

    // addChild is called by a newly constructed AbstractLedgerTxn to become a
    // child of AbstractLedgerTxnParent. Throws if AbstractLedgerTxnParent
    // is in the sealed state or already has a child.
    virtual void addChild(AbstractLedgerTxn& child) = 0;

    // commitChild and rollbackChild are called by a child AbstractLedgerTxn
    // to trigger an atomic commit or an atomic rollback of the data stored in
    // the child.
    virtual void commitChild(EntryIterator iter) = 0;
    virtual void rollbackChild() = 0;

    // getAllOffers, getBestOffer, and getOffersByAccountAndAsset are used to
    // handle some specific queries related to Offers.
    // - getAllOffers
    //     Get XDR for every offer, grouped by account.
    // - getBestOffer
    //     Get XDR for the best offer with specified buying and selling assets.
    // - getOffersByAccountAndAsset
    //     Get XDR for every offer owned by the specified account that is either
    //     buying or selling the specified asset.
    virtual std::unordered_map<LedgerKey, LedgerEntry> getAllOffers() = 0;
    virtual std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::unordered_set<LedgerKey>& exclude) = 0;
    virtual std::unordered_map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) = 0;

    // getHeader returns the LedgerHeader stored by AbstractLedgerTxnParent.
    // Used to allow the LedgerHeader to propagate to a child.
    virtual LedgerHeader const& getHeader() const = 0;

    // getInflationWinners is used to handle the specific queries related to
    // inflation. Returns a maximum of maxWinners winners, each of which has a
    // minimum of minBalance votes.
    virtual std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) = 0;

    // getNewestVersion finds the newest version of the LedgerEntry associated
    // with the LedgerKey key by checking if there is a version stored in this
    // AbstractLedgerTxnParent, and if not recursively invoking
    // getNewestVersion on its parent. Returns nullptr if the key does not exist
    // or if the corresponding LedgerEntry has been erased.
    virtual std::shared_ptr<LedgerEntry const>
    getNewestVersion(LedgerKey const& key) const = 0;
};

// An abstraction for an object that is an AbstractLedgerTxnParent and has
// transaction semantics. AbstractLedgerTxns manage LedgerTxnEntry and
// LedgerTxnHeader objects to allow data to be created, modified, and erased.
class AbstractLedgerTxn : public AbstractLedgerTxnParent
{
    // deactivate is used to deactivate the LedgerTxnEntry associated with the
    // given key.
    friend class LedgerTxnEntry::Impl;
    friend class ConstLedgerTxnEntry::Impl;
    virtual void deactivate(LedgerKey const& key) = 0;

    // deactivateHeader is used to deactivate the LedgerTxnHeader.
    friend class LedgerTxnHeader::Impl;
    virtual void deactivateHeader() = 0;

  public:
    // Automatically rollback the data stored in the AbstractLedgerTxn if it
    // has not already been committed or rolled back.
    virtual ~AbstractLedgerTxn();

    // commit and rollback trigger an atomic commit into the parent or an atomic
    // rollback of the data stored in the AbstractLedgerTxn.
    virtual void commit() = 0;
    virtual void rollback() = 0;

    // loadHeader, create, erase, load, and loadWithoutRecord provide the main
    // interface to interact with data stored in the AbstractLedgerTxn. These
    // functions only allow one instance of a particular data to be active at a
    // time.
    // - loadHeader
    //     Loads the current LedgerHeader. Throws if there is already an active
    //     LedgerTxnHeader.
    // - create
    //     Creates a new LedgerTxnEntry from entry. Throws if the key
    //     associated with this entry is already associated with an entry in
    //     this AbstractLedgerTxn or any parent.
    // - erase
    //     Erases the existing LedgerEntry associated with key. Throws if the
    //     key is not already associated with an entry in this
    //     AbstractLedgerTxn or any parent. Throws if there is an active
    //     LedgerTxnEntry associated with this key.
    // - load:
    //     Loads a LedgerEntry by key. Returns nullptr if the key is not
    //     associated with an entry in this AbstractLedgerTxn or in any
    //     parent. Throws if there is an active LedgerTxnEntry associated with
    //     this key.
    // - loadWithoutRecord:
    //     Similar to load, but the load is not recorded (meaning that it does
    //     not lead to a LIVE entry in the bucket list) and the loaded data is
    //     const as a consequence. Note that if the key was already recorded
    //     then it will still be recorded after calling loadWithoutRecord.
    //     Throws if there is an active LedgerTxnEntry associated with this
    //     key.
    // All of these functions throw if the AbstractLedgerTxn is sealed or if
    // the AbstractLedgerTxn has a child.
    virtual LedgerTxnHeader loadHeader() = 0;
    virtual LedgerTxnEntry create(LedgerEntry const& entry) = 0;
    virtual void erase(LedgerKey const& key) = 0;
    virtual LedgerTxnEntry load(LedgerKey const& key) = 0;
    virtual ConstLedgerTxnEntry loadWithoutRecord(LedgerKey const& key) = 0;

    // getChanges, getDelta, getDeadEntries, and getLiveEntries are used to
    // extract information about changes contained in the AbstractLedgerTxn
    // in different formats. These functions also cause the AbstractLedgerTxn
    // to enter the sealed state, simultaneously updating last modified if
    // necessary.
    // - getChanges
    //     Extract all changes from this AbstractLedgerTxn in XDR format. To
    //     be stored as meta.
    // - getDelta
    //     Extract all changes from this AbstractLedgerTxn (including changes
    //     to the LedgerHeader) in a format convenient for answering queries
    //     about how specific entries and the header have changed. To be used
    //     for invariants.
    // - getDeadEntries and getLiveEntries
    //     getDeadEntries extracts a list of keys that are now dead, whereas
    //     getLiveEntries extracts a list of entries that were recorded and
    //     are still alive. To be inserted into the BucketList.
    // All of these functions throw if the AbstractLedgerTxn has a child.
    virtual LedgerEntryChanges getChanges() = 0;
    virtual LedgerTxnDelta getDelta() = 0;
    virtual std::vector<LedgerKey> getDeadEntries() = 0;
    virtual std::vector<LedgerEntry> getLiveEntries() = 0;

    // loadAllOffers, loadBestOffer, and loadOffersByAccountAndAsset are used to
    // handle some specific queries related to Offers. These functions are built
    // on top of load, and so share many properties with that function.
    // - loadAllOffers
    //     Load every offer, grouped by account.
    // - loadBestOffer
    //     Load the best offer with specified buying and selling assets.
    // - loadOffersByAccountAndAsset
    //     Load every offer owned by the specified account that is either buying
    //     or selling the specified asset.
    // All of these functions throw if the AbstractLedgerTxn is sealed or if
    // the AbstractLedgerTxn has a child. These functions also throw if any
    // LedgerKey they try to load is already active.
    virtual std::map<AccountID, std::vector<LedgerTxnEntry>>
    loadAllOffers() = 0;
    virtual LedgerTxnEntry loadBestOffer(Asset const& buying,
                                         Asset const& selling) = 0;
    virtual std::vector<LedgerTxnEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) = 0;

    // queryInflationWinners is a wrapper around getInflationWinners that throws
    // if the AbstractLedgerTxn is sealed or if the AbstractLedgerTxn has a
    // child.
    virtual std::vector<InflationWinner>
    queryInflationWinners(size_t maxWinners, int64_t minBalance) = 0;

    // unsealHeader is used to modify the LedgerHeader after AbstractLedgerTxn
    // has entered the sealed state. This is required to update bucketListHash,
    // which can only be done after getDeadEntries and getLiveEntries have been
    // called.
    virtual void unsealHeader(std::function<void(LedgerHeader&)> f) = 0;
};

class LedgerTxn final : public AbstractLedgerTxn
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

    void deactivate(LedgerKey const& key) override;

    void deactivateHeader() override;

    std::unique_ptr<Impl> const& getImpl() const;

  public:
    explicit LedgerTxn(AbstractLedgerTxnParent& parent,
                       bool shouldUpdateLastModified = true);
    explicit LedgerTxn(LedgerTxn& parent, bool shouldUpdateLastModified = true);

    virtual ~LedgerTxn();

    void addChild(AbstractLedgerTxn& child) override;

    void commit() override;

    void commitChild(EntryIterator iter) override;

    LedgerTxnEntry create(LedgerEntry const& entry) override;

    void erase(LedgerKey const& key) override;

    std::unordered_map<LedgerKey, LedgerEntry> getAllOffers() override;

    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::unordered_set<LedgerKey>& exclude) override;

    LedgerEntryChanges getChanges() override;

    std::vector<LedgerKey> getDeadEntries() override;

    LedgerTxnDelta getDelta() override;

    std::unordered_map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    LedgerHeader const& getHeader() const override;

    std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::vector<InflationWinner>
    queryInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::vector<LedgerEntry> getLiveEntries() override;

    std::shared_ptr<LedgerEntry const>
    getNewestVersion(LedgerKey const& key) const override;

    LedgerTxnEntry load(LedgerKey const& key) override;

    std::map<AccountID, std::vector<LedgerTxnEntry>> loadAllOffers() override;

    LedgerTxnEntry loadBestOffer(Asset const& buying,
                                 Asset const& selling) override;

    LedgerTxnHeader loadHeader() override;

    std::vector<LedgerTxnEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) override;

    ConstLedgerTxnEntry loadWithoutRecord(LedgerKey const& key) override;

    void rollback() override;

    void rollbackChild() override;

    void unsealHeader(std::function<void(LedgerHeader&)> f) override;
};

class LedgerTxnRoot : public AbstractLedgerTxnParent
{
    class Impl;
    std::unique_ptr<Impl> const mImpl;

  public:
    explicit LedgerTxnRoot(Database& db, size_t entryCacheSize = 4096,
                           size_t bestOfferCacheSize = 64);

    virtual ~LedgerTxnRoot();

    void addChild(AbstractLedgerTxn& child) override;

    void commitChild(EntryIterator iter) override;

    uint64_t countObjects(LedgerEntryType let) const;
    uint64_t countObjects(LedgerEntryType let,
                          LedgerRange const& ledgers) const;

    void deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const;

    void dropAccounts();
    void dropData();
    void dropOffers();
    void dropTrustLines();

    std::unordered_map<LedgerKey, LedgerEntry> getAllOffers() override;

    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::unordered_set<LedgerKey>& exclude) override;

    std::unordered_map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    LedgerHeader const& getHeader() const override;

    std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::shared_ptr<LedgerEntry const>
    getNewestVersion(LedgerKey const& key) const override;

    void rollbackChild() override;
};
}
