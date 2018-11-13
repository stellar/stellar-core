#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "xdr/Stellar-ledger.h"
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace stellar
{

class Database;
struct InflationVotes;
struct LedgerEntry;
struct LedgerKey;
class LedgerRange;

bool isBetterOffer(LedgerEntry const& lhsEntry, LedgerEntry const& rhsEntry);

class AbstractLedgerState;

struct InflationWinner
{
    AccountID accountID;
    int64_t votes;
};

// LedgerStateDelta represents the difference between a LedgerState and its
// parent. Used in the Invariants subsystem.
struct LedgerStateDelta
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

    std::map<LedgerKey, EntryDelta> entry;
    HeaderDelta header;
};

// An abstraction for an object that is iterator-like and permits enumerating
// the LedgerStateEntry objects managed by an AbstractLedgerState. This enables
// an AbstractLedgerStateParent to iterate over the entries managed by its child
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

// An abstraction for an object that can be the parent of an AbstractLedgerState
// (discussed below). Allows children to commit atomically to the parent. Has no
// notion of a LedgerStateEntry or LedgerStateHeader (discussed respectively in
// LedgerStateEntry.h and LedgerStateHeader.h) but allows access to XDR objects
// such as LedgerEntry and LedgerHeader. This interface is designed such that
// concrete implementations can be databases or AbstractLedgerState objects. In
// general, this interface was not designed to be used directly by end users.
// Rather, end users should interact with AbstractLedgerStateParent through the
// AbstractLedgerState interface.
class AbstractLedgerStateParent
{
  public:
    virtual ~AbstractLedgerStateParent();

    // addChild is called by a newly constructed AbstractLedgerState to become a
    // child of AbstractLedgerStateParent. Throws if AbstractLedgerStateParent
    // is in the sealed state or already has a child.
    virtual void addChild(AbstractLedgerState& child) = 0;

    // commitChild and rollbackChild are called by a child AbstractLedgerState
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
    virtual std::map<LedgerKey, LedgerEntry> getAllOffers() = 0;
    virtual std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::set<LedgerKey>& exclude) = 0;
    virtual std::map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) = 0;

    // getHeader returns the LedgerHeader stored by AbstractLedgerStateParent.
    // Used to allow the LedgerHeader to propagate to a child.
    virtual LedgerHeader const& getHeader() const = 0;

    // getInflationWinners is used to handle the specific queries related to
    // inflation. Returns a maximum of maxWinners winners, each of which has a
    // minimum of minBalance votes.
    virtual std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) = 0;

    // getNewestVersion finds the newest version of the LedgerEntry associated
    // with the LedgerKey key by checking if there is a version stored in this
    // AbstractLedgerStateParent, and if not recursively invoking
    // getNewestVersion on its parent. Returns nullptr if the key does not exist
    // or if the corresponding LedgerEntry has been erased.
    virtual std::shared_ptr<LedgerEntry const>
    getNewestVersion(LedgerKey const& key) const = 0;
};

// An abstraction for an object that is an AbstractLedgerStateParent and has
// transaction semantics. AbstractLedgerStates manage LedgerStateEntry and
// LedgerStateHeader objects to allow data to be created, modified, and erased.
class AbstractLedgerState : public AbstractLedgerStateParent
{
    // deactivate is used to deactivate the LedgerStateEntry associated with the
    // given key.
    friend class LedgerStateEntry::Impl;
    friend class ConstLedgerStateEntry::Impl;
    virtual void deactivate(LedgerKey const& key) = 0;

    // deactivateHeader is used to deactivate the LedgerStateHeader.
    friend class LedgerStateHeader::Impl;
    virtual void deactivateHeader() = 0;

  public:
    // Automatically rollback the data stored in the AbstractLedgerState if it
    // has not already been committed or rolled back.
    virtual ~AbstractLedgerState();

    // commit and rollback trigger an atomic commit into the parent or an atomic
    // rollback of the data stored in the AbstractLedgerState.
    virtual void commit() = 0;
    virtual void rollback() = 0;

    // loadHeader, create, erase, load, and loadWithoutRecord provide the main
    // interface to interact with data stored in the AbstractLedgerState. These
    // functions only allow one instance of a particular data to be active at a
    // time.
    // - loadHeader
    //     Loads the current LedgerHeader. Throws if there is already an active
    //     LedgerStateHeader.
    // - create
    //     Creates a new LedgerStateEntry from entry. Throws if the key
    //     associated with this entry is already associated with an entry in
    //     this AbstractLedgerState or any parent.
    // - erase
    //     Erases the existing LedgerEntry associated with key. Throws if the
    //     key is not already associated with an entry in this
    //     AbstractLedgerState or any parent. Throws if there is an active
    //     LedgerStateEntry associated with this key.
    // - load:
    //     Loads a LedgerEntry by key. Returns nullptr if the key is not
    //     associated with an entry in this AbstractLedgerState or in any
    //     parent. Throws if there is an active LedgerStateEntry associated with
    //     this key.
    // - loadWithoutRecord:
    //     Similar to load, but the load is not recorded (meaning that it does
    //     not lead to a LIVE entry in the bucket list) and the loaded data is
    //     const as a consequence. Note that if the key was already recorded
    //     then it will still be recorded after calling loadWithoutRecord.
    //     Throws if there is an active LedgerStateEntry associated with this
    //     key.
    // All of these functions throw if the AbstractLedgerState is sealed or if
    // the AbstractLedgerState has a child.
    virtual LedgerStateHeader loadHeader() = 0;
    virtual LedgerStateEntry create(LedgerEntry const& entry) = 0;
    virtual void erase(LedgerKey const& key) = 0;
    virtual LedgerStateEntry load(LedgerKey const& key) = 0;
    virtual ConstLedgerStateEntry loadWithoutRecord(LedgerKey const& key) = 0;

    // getChanges, getDelta, getDeadEntries, and getLiveEntries are used to
    // extract information about changes contained in the AbstractLedgerState
    // in different formats. These functions also cause the AbstractLedgerState
    // to enter the sealed state, simultaneously updating last modified if
    // necessary.
    // - getChanges
    //     Extract all changes from this AbstractLedgerState in XDR format. To
    //     be stored as meta.
    // - getDelta
    //     Extract all changes from this AbstractLedgerState (including changes
    //     to the LedgerHeader) in a format convenient for answering queries
    //     about how specific entries and the header have changed. To be used
    //     for invariants.
    // - getDeadEntries and getLiveEntries
    //     getDeadEntries extracts a list of keys that are now dead, whereas
    //     getLiveEntries extracts a list of entries that were recorded and
    //     are still alive. To be inserted into the BucketList.
    // All of these functions throw if the AbstractLedgerState has a child.
    virtual LedgerEntryChanges getChanges() = 0;
    virtual LedgerStateDelta getDelta() = 0;
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
    // All of these functions throw if the AbstractLedgerState is sealed or if
    // the AbstractLedgerState has a child. These functions also throw if any
    // LedgerKey they try to load is already active.
    virtual std::map<AccountID, std::vector<LedgerStateEntry>>
    loadAllOffers() = 0;
    virtual LedgerStateEntry loadBestOffer(Asset const& buying,
                                           Asset const& selling) = 0;
    virtual std::vector<LedgerStateEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) = 0;

    // queryInflationWinners is a wrapper around getInflationWinners that throws
    // if the AbstractLedgerState is sealed or if the AbstractLedgerState has a
    // child.
    virtual std::vector<InflationWinner>
    queryInflationWinners(size_t maxWinners, int64_t minBalance) = 0;

    // unsealHeader is used to modify the LedgerHeader after AbstractLedgerState
    // has entered the sealed state. This is required to update bucketListHash,
    // which can only be done after getDeadEntries and getLiveEntries have been
    // called.
    virtual void unsealHeader(std::function<void(LedgerHeader&)> f) = 0;
};

class LedgerState final : public AbstractLedgerState
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

    void deactivate(LedgerKey const& key) override;

    void deactivateHeader() override;

    std::unique_ptr<Impl> const& getImpl() const;

  public:
    explicit LedgerState(AbstractLedgerStateParent& parent,
                         bool shouldUpdateLastModified = true);
    explicit LedgerState(LedgerState& parent,
                         bool shouldUpdateLastModified = true);

    virtual ~LedgerState();

    void addChild(AbstractLedgerState& child) override;

    void commit() override;

    void commitChild(EntryIterator iter) override;

    LedgerStateEntry create(LedgerEntry const& entry) override;

    void erase(LedgerKey const& key) override;

    std::map<LedgerKey, LedgerEntry> getAllOffers() override;

    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::set<LedgerKey>& exclude) override;

    LedgerEntryChanges getChanges() override;

    std::vector<LedgerKey> getDeadEntries() override;

    LedgerStateDelta getDelta() override;

    std::map<LedgerKey, LedgerEntry>
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

    LedgerStateEntry load(LedgerKey const& key) override;

    std::map<AccountID, std::vector<LedgerStateEntry>> loadAllOffers() override;

    LedgerStateEntry loadBestOffer(Asset const& buying,
                                   Asset const& selling) override;

    LedgerStateHeader loadHeader() override;

    std::vector<LedgerStateEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) override;

    ConstLedgerStateEntry loadWithoutRecord(LedgerKey const& key) override;

    void rollback() override;

    void rollbackChild() override;

    void unsealHeader(std::function<void(LedgerHeader&)> f) override;
};

class LedgerStateRoot : public AbstractLedgerStateParent
{
    class Impl;
    std::unique_ptr<Impl> const mImpl;

  public:
    explicit LedgerStateRoot(Database& db, size_t entryCacheSize = 4096,
                             size_t bestOfferCacheSize = 64);

    virtual ~LedgerStateRoot();

    void addChild(AbstractLedgerState& child) override;

    void commitChild(EntryIterator iter) override;

    uint64_t countObjects(LedgerEntryType let) const;
    uint64_t countObjects(LedgerEntryType let,
                          LedgerRange const& ledgers) const;

    void deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const;

    void dropAccounts();
    void dropData();
    void dropOffers();
    void dropTrustLines();

    std::map<LedgerKey, LedgerEntry> getAllOffers() override;

    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 std::set<LedgerKey>& exclude) override;

    std::map<LedgerKey, LedgerEntry>
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
