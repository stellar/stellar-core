// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/GlobalChecks.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/marshal.h"
#include <soci.h>

namespace stellar
{

// Implementation of AbstractLedgerTxnParent --------------------------------
AbstractLedgerTxnParent::~AbstractLedgerTxnParent()
{
}

// Implementation of EntryIterator --------------------------------------------
EntryIterator::EntryIterator(std::unique_ptr<AbstractImpl>&& impl)
    : mImpl(std::move(impl))
{
}

EntryIterator::EntryIterator(EntryIterator&& other)
    : mImpl(std::move(other.mImpl))
{
}

std::unique_ptr<EntryIterator::AbstractImpl> const&
EntryIterator::getImpl() const
{
    if (!mImpl)
    {
        throw std::runtime_error("Iterator is empty");
    }
    return mImpl;
}

EntryIterator& EntryIterator::operator++()
{
    getImpl()->advance();
    return *this;
}

EntryIterator::operator bool() const
{
    return !getImpl()->atEnd();
}

LedgerEntry const&
EntryIterator::entry() const
{
    return getImpl()->entry();
}

bool
EntryIterator::entryExists() const
{
    return getImpl()->entryExists();
}

LedgerKey const&
EntryIterator::key() const
{
    return getImpl()->key();
}

// Implementation of AbstractLedgerTxn --------------------------------------
AbstractLedgerTxn::~AbstractLedgerTxn()
{
}

// Implementation of LedgerTxn ----------------------------------------------
LedgerTxn::LedgerTxn(AbstractLedgerTxnParent& parent,
                     bool shouldUpdateLastModified)
    : mImpl(std::make_unique<Impl>(*this, parent, shouldUpdateLastModified))
{
}

LedgerTxn::LedgerTxn(LedgerTxn& parent, bool shouldUpdateLastModified)
    : LedgerTxn((AbstractLedgerTxnParent&)parent, shouldUpdateLastModified)
{
}

LedgerTxn::Impl::Impl(LedgerTxn& self, AbstractLedgerTxnParent& parent,
                      bool shouldUpdateLastModified)
    : mParent(parent)
    , mChild(nullptr)
    , mHeader(std::make_unique<LedgerHeader>(mParent.getHeader()))
    , mShouldUpdateLastModified(shouldUpdateLastModified)
    , mIsSealed(false)
{
    mParent.addChild(self);
}

LedgerTxn::~LedgerTxn()
{
    if (mImpl)
    {
        rollback();
    }
}

std::unique_ptr<LedgerTxn::Impl> const&
LedgerTxn::getImpl() const
{
    if (!mImpl)
    {
        throw std::runtime_error("LedgerTxnEntry was handled");
    }
    return mImpl;
}

void
LedgerTxn::addChild(AbstractLedgerTxn& child)
{
    getImpl()->addChild(child);
}

void
LedgerTxn::Impl::addChild(AbstractLedgerTxn& child)
{
    throwIfSealed();
    throwIfChild();

    mChild = &child;

    // std::set<...>::clear is noexcept
    mActive.clear();

    // std::shared_ptr<...>::reset is noexcept
    mActiveHeader.reset();
}

void
LedgerTxn::Impl::throwIfChild() const
{
    if (mChild)
    {
        throw std::runtime_error("LedgerTxn has child");
    }
}

void
LedgerTxn::Impl::throwIfSealed() const
{
    if (mIsSealed)
    {
        throw std::runtime_error("LedgerTxn is sealed");
    }
}

void
LedgerTxn::commit()
{
    getImpl()->commit();
    mImpl.reset();
}

void
LedgerTxn::Impl::commit()
{
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        // getEntryIterator has the strong exception safety guarantee
        // commitChild has the strong exception safety guarantee
        mParent.commitChild(getEntryIterator(entries));
    });
}

void
LedgerTxn::commitChild(EntryIterator iter)
{
    getImpl()->commitChild(std::move(iter));
}

void
LedgerTxn::Impl::commitChild(EntryIterator iter)
{
    // Assignment of xdrpp objects does not have the strong exception safety
    // guarantee, so use std::unique_ptr<...>::swap to achieve it
    auto childHeader = std::make_unique<LedgerHeader>(mChild->getHeader());

    try
    {
        for (; (bool)iter; ++iter)
        {
            auto const& key = iter.key();
            if (iter.entryExists())
            {
                mEntry[key] = std::make_shared<LedgerEntry>(iter.entry());
            }
            else if (!mParent.getNewestVersion(key))
            { // Created in this LedgerTxn
                mEntry.erase(key);
            }
            else
            { // Existed in a previous LedgerTxn
                mEntry[key] = nullptr;
            }
        }
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error during commit to LedgerTxn: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error during commit to LedgerTxn");
    }

    // std::unique_ptr<...>::swap does not throw
    mHeader.swap(childHeader);
    mChild = nullptr;
}

LedgerTxnEntry
LedgerTxn::create(LedgerEntry const& entry)
{
    return getImpl()->create(*this, entry);
}

LedgerTxnEntry
LedgerTxn::Impl::create(LedgerTxn& self, LedgerEntry const& entry)
{
    throwIfSealed();
    throwIfChild();

    auto key = LedgerEntryKey(entry);
    if (getNewestVersion(key))
    {
        throw std::runtime_error("Key already exists");
    }

    auto current = std::make_shared<LedgerEntry>(entry);
    auto impl = LedgerTxnEntry::makeSharedImpl(self, *current);

    // Set the key to active before constructing the LedgerTxnEntry, as this
    // can throw and the LedgerTxnEntry destructor requires that mActive
    // contains key. LedgerTxnEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerTxnEntry ltxe(impl);

    // std::shared_ptr assignment is noexcept
    mEntry[key] = current;
    return ltxe;
}

void
LedgerTxn::deactivate(LedgerKey const& key)
{
    getImpl()->deactivate(key);
}

void
LedgerTxn::Impl::deactivate(LedgerKey const& key)
{
    auto iter = mActive.find(key);
    if (iter == mActive.end())
    {
        throw std::runtime_error("Key is not active");
    }
    mActive.erase(iter);
}

void
LedgerTxn::deactivateHeader()
{
    getImpl()->deactivateHeader();
}

void
LedgerTxn::Impl::deactivateHeader()
{
    if (!mActiveHeader)
    {
        throw std::runtime_error("LedgerTxnHeader is not active");
    }
    mActiveHeader.reset();
}

void
LedgerTxn::erase(LedgerKey const& key)
{
    getImpl()->erase(key);
}

void
LedgerTxn::Impl::erase(LedgerKey const& key)
{
    throwIfSealed();
    throwIfChild();

    auto newest = getNewestVersion(key);
    if (!newest)
    {
        throw std::runtime_error("Key does not exist");
    }

    auto activeIter = mActive.find(key);
    bool isActive = activeIter != mActive.end();

    if (!mParent.getNewestVersion(key))
    { // Created in this LedgerTxn
        mEntry.erase(key);
    }
    else
    { // Existed in a previous LedgerTxn
        auto iter = mEntry.find(key);
        if (iter != mEntry.end())
        {
            iter->second.reset();
        }
        else
        {
            // C++14 requirements for exception safety of associative containers
            // guarantee that if emplace throws when inserting a single element
            // then the insertion has no effect
            mEntry.emplace(key, nullptr);
        }
    }
    // Note: Cannot throw after this point because the entry will not be
    // deactivated in that case

    if (isActive)
    {
        // C++14 requirements for exception safety of containers guarantee that
        // erase(iter) does not throw
        mActive.erase(activeIter);
    }
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxn::getAllOffers()
{
    return getImpl()->getAllOffers();
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxn::Impl::getAllOffers()
{
    auto offers = mParent.getAllOffers();
    for (auto const& kv : mEntry)
    {
        auto const& key = kv.first;
        auto const& entry = kv.second;
        if (key.type() != OFFER)
        {
            continue;
        }
        if (!entry)
        {
            offers.erase(key);
            continue;
        }
        offers[key] = *entry;
    }
    return offers;
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::getBestOffer(Asset const& buying, Asset const& selling,
                        std::unordered_set<LedgerKey>& exclude)
{
    return getImpl()->getBestOffer(buying, selling, exclude);
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::Impl::getBestOffer(Asset const& buying, Asset const& selling,
                              std::unordered_set<LedgerKey>& exclude)
{
    auto end = mEntry.cend();
    auto bestOfferIter = end;
    if (exclude.size() + mEntry.size() > exclude.bucket_count())
    {
        // taking a best guess as a starting point for how big exclude can get
        // this avoids rehashing when processing transactions after a lot of
        // changes already occured
        exclude.reserve(exclude.size() + mEntry.size());
    }
    for (auto iter = mEntry.cbegin(); iter != end; ++iter)
    {
        auto const& key = iter->first;
        auto const& entry = iter->second;
        if (key.type() != OFFER)
        {
            continue;
        }

        if (!exclude.insert(key).second)
        {
            continue;
        }

        if (!(entry && entry->data.offer().buying == buying &&
              entry->data.offer().selling == selling))
        {
            continue;
        }

        if ((bestOfferIter == end) ||
            isBetterOffer(*entry, *bestOfferIter->second))
        {
            bestOfferIter = iter;
        }
    }

    std::shared_ptr<LedgerEntry const> bestOffer;
    if (bestOfferIter != end)
    {
        bestOffer = std::make_shared<LedgerEntry const>(*bestOfferIter->second);
    }

    auto parentBestOffer = mParent.getBestOffer(buying, selling, exclude);
    if (bestOffer && parentBestOffer)
    {
        return isBetterOffer(*bestOffer, *parentBestOffer) ? bestOffer
                                                           : parentBestOffer;
    }
    else
    {
        return bestOffer ? bestOffer : parentBestOffer;
    }
}

LedgerEntryChanges
LedgerTxn::getChanges()
{
    return getImpl()->getChanges();
}

LedgerEntryChanges
LedgerTxn::Impl::getChanges()
{
    LedgerEntryChanges changes;
    changes.reserve(mEntry.size() * 2);
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& key = kv.first;
            auto const& entry = kv.second;

            auto previous = mParent.getNewestVersion(key);
            if (previous)
            {
                changes.emplace_back(LEDGER_ENTRY_STATE);
                changes.back().state() = *previous;

                if (entry)
                {
                    changes.emplace_back(LEDGER_ENTRY_UPDATED);
                    changes.back().updated() = *entry;
                }
                else
                {
                    changes.emplace_back(LEDGER_ENTRY_REMOVED);
                    changes.back().removed() = key;
                }
            }
            else
            {
                // If !entry and !previous.entry then the entry was created and
                // erased in this LedgerTxn, in which case it should not still
                // be in this LedgerTxn
                assert(entry);
                changes.emplace_back(LEDGER_ENTRY_CREATED);
                changes.back().created() = *entry;
            }
        }
    });
    return changes;
}

std::vector<LedgerKey>
LedgerTxn::getDeadEntries()
{
    return getImpl()->getDeadEntries();
}

std::vector<LedgerKey>
LedgerTxn::Impl::getDeadEntries()
{
    std::vector<LedgerKey> res;
    res.reserve(mEntry.size());
    maybeUpdateLastModifiedThenInvokeThenSeal([&res](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& key = kv.first;
            auto const& entry = kv.second;
            if (!entry)
            {
                res.push_back(key);
            }
        }
    });
    return res;
}

LedgerTxnDelta
LedgerTxn::getDelta()
{
    return getImpl()->getDelta();
}

LedgerTxnDelta
LedgerTxn::Impl::getDelta()
{
    LedgerTxnDelta delta;
    delta.entry.reserve(mEntry.size());
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& key = kv.first;
            auto previous = mParent.getNewestVersion(key);

            // Deep copy is not required here because getDelta causes
            // LedgerTxn to enter the sealed state, meaning subsequent
            // modifications are impossible.
            delta.entry[key] = {kv.second, previous};
        }
        delta.header = {*mHeader, mParent.getHeader()};
    });
    return delta;
}

EntryIterator
LedgerTxn::Impl::getEntryIterator(EntryMap const& entries) const
{
    auto iterImpl =
        std::make_unique<EntryIteratorImpl>(entries.cbegin(), entries.cend());
    return EntryIterator(std::move(iterImpl));
}

LedgerHeader const&
LedgerTxn::getHeader() const
{
    return getImpl()->getHeader();
}

LedgerHeader const&
LedgerTxn::Impl::getHeader() const
{
    return *mHeader;
}

std::vector<InflationWinner>
LedgerTxn::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    return getImpl()->getInflationWinners(maxWinners, minVotes);
}

std::map<AccountID, int64_t>
LedgerTxn::Impl::getDeltaVotes() const
{
    int64_t const MIN_VOTES_TO_INCLUDE = 1000000000;
    std::map<AccountID, int64_t> deltaVotes;
    for (auto const& kv : mEntry)
    {
        auto const& key = kv.first;
        auto const& entry = kv.second;
        if (key.type() != ACCOUNT)
        {
            continue;
        }

        if (entry)
        {
            auto const& acc = entry->data.account();
            if (acc.inflationDest && acc.balance >= MIN_VOTES_TO_INCLUDE)
            {
                deltaVotes[*acc.inflationDest] += acc.balance;
            }
        }

        auto previous = mParent.getNewestVersion(key);
        if (previous)
        {
            auto const& acc = previous->data.account();
            if (acc.inflationDest && acc.balance >= MIN_VOTES_TO_INCLUDE)
            {
                deltaVotes[*acc.inflationDest] -= acc.balance;
            }
        }
    }
    return deltaVotes;
}

std::map<AccountID, int64_t>
LedgerTxn::Impl::getTotalVotes(
    std::vector<InflationWinner> const& parentWinners,
    std::map<AccountID, int64_t> const& deltaVotes, int64_t minVotes) const
{
    std::map<AccountID, int64_t> totalVotes;
    for (auto const& winner : parentWinners)
    {
        totalVotes.emplace(winner.accountID, winner.votes);
    }
    for (auto const& delta : deltaVotes)
    {
        auto const& accountID = delta.first;
        auto const& voteDelta = delta.second;
        if ((totalVotes.find(accountID) != totalVotes.end()) ||
            voteDelta >= minVotes)
        {
            totalVotes[accountID] += voteDelta;
        }
    }
    return totalVotes;
}

std::vector<InflationWinner>
LedgerTxn::Impl::enumerateInflationWinners(
    std::map<AccountID, int64_t> const& totalVotes, size_t maxWinners,
    int64_t minVotes) const
{
    std::vector<InflationWinner> winners;
    for (auto const& total : totalVotes)
    {
        auto const& accountID = total.first;
        auto const& voteTotal = total.second;
        if (voteTotal >= minVotes)
        {
            winners.push_back({accountID, voteTotal});
        }
    }

    // Sort the new winners and remove the excess
    std::sort(winners.begin(), winners.end(),
              [](auto const& lhs, auto const& rhs) {
                  if (lhs.votes == rhs.votes)
                  {
                      return KeyUtils::toStrKey(lhs.accountID) >
                             KeyUtils::toStrKey(rhs.accountID);
                  }
                  return lhs.votes > rhs.votes;
              });
    if (winners.size() > maxWinners)
    {
        winners.resize(maxWinners);
    }
    return winners;
}

std::vector<InflationWinner>
LedgerTxn::Impl::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    // Calculate vote changes relative to parent
    auto deltaVotes = getDeltaVotes();

    // Have to load extra winners corresponding to the number of accounts that
    // have had their vote totals change
    size_t numChanged =
        std::count_if(deltaVotes.cbegin(), deltaVotes.cend(),
                      [](auto const& val) { return val.second != 0; });
    // Equivalent to maxWinners + numChanged > MAX
    if (std::numeric_limits<size_t>::max() - numChanged < maxWinners)
    {
        throw std::runtime_error("max winners overflowed");
    }
    size_t newMaxWinners = maxWinners + numChanged;

    // Have to load accounts that could be winners after accounting for the
    // change in their vote totals
    auto maxIncreaseIter =
        std::max_element(deltaVotes.cbegin(), deltaVotes.cend(),
                         [](auto const& lhs, auto const& rhs) {
                             return lhs.second < rhs.second;
                         });
    int64_t maxIncrease = (maxIncreaseIter != deltaVotes.cend())
                              ? std::max<int64_t>(0, maxIncreaseIter->second)
                              : 0;
    int64_t newMinVotes =
        (minVotes > maxIncrease) ? (minVotes - maxIncrease) : 0;

    // Get winners from parent, update votes, and add potential new winners
    // Note: It is possible that there are new winners in the case where an
    // account was receiving no votes before this ledger but now some accounts
    // are voting for it
    auto totalVotes =
        getTotalVotes(mParent.getInflationWinners(newMaxWinners, newMinVotes),
                      deltaVotes, minVotes);

    // Enumerate the new winners in sorted order
    return enumerateInflationWinners(totalVotes, maxWinners, minVotes);
}

std::vector<InflationWinner>
LedgerTxn::queryInflationWinners(size_t maxWinners, int64_t minVotes)
{
    return getImpl()->queryInflationWinners(maxWinners, minVotes);
}

std::vector<InflationWinner>
LedgerTxn::Impl::queryInflationWinners(size_t maxWinners, int64_t minVotes)
{
    throwIfSealed();
    throwIfChild();
    return getInflationWinners(maxWinners, minVotes);
}

std::vector<LedgerEntry>
LedgerTxn::getLiveEntries()
{
    return getImpl()->getLiveEntries();
}

std::vector<LedgerEntry>
LedgerTxn::Impl::getLiveEntries()
{
    std::vector<LedgerEntry> res;
    res.reserve(mEntry.size());
    maybeUpdateLastModifiedThenInvokeThenSeal([&res](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& entry = kv.second;
            if (entry)
            {
                res.push_back(*entry);
            }
        }
    });
    return res;
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::getNewestVersion(LedgerKey const& key) const
{
    return getImpl()->getNewestVersion(key);
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::Impl::getNewestVersion(LedgerKey const& key) const
{
    auto iter = mEntry.find(key);
    if (iter != mEntry.end())
    {
        return iter->second;
    }
    return mParent.getNewestVersion(key);
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxn::getOffersByAccountAndAsset(AccountID const& account,
                                      Asset const& asset)
{
    return getImpl()->getOffersByAccountAndAsset(account, asset);
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxn::Impl::getOffersByAccountAndAsset(AccountID const& account,
                                            Asset const& asset)
{
    auto offers = mParent.getOffersByAccountAndAsset(account, asset);
    for (auto const& kv : mEntry)
    {
        auto const& key = kv.first;
        auto const& entry = kv.second;
        if (key.type() != OFFER)
        {
            continue;
        }
        if (!entry)
        {
            offers.erase(key);
            continue;
        }

        auto const& oe = entry->data.offer();
        if (oe.sellerID == account &&
            (oe.selling == asset || oe.buying == asset))
        {
            offers[key] = *entry;
        }
        else
        {
            offers.erase(key);
        }
    }
    return offers;
}

LedgerTxnEntry
LedgerTxn::load(LedgerKey const& key)
{
    return getImpl()->load(*this, key);
}

LedgerTxnEntry
LedgerTxn::Impl::load(LedgerTxn& self, LedgerKey const& key)
{
    throwIfSealed();
    throwIfChild();
    if (mActive.find(key) != mActive.end())
    {
        throw std::runtime_error("Key is active");
    }

    auto newest = getNewestVersion(key);
    if (!newest)
    {
        return {};
    }

    auto current = std::make_shared<LedgerEntry>(*newest);
    auto impl = LedgerTxnEntry::makeSharedImpl(self, *current);

    // Set the key to active before constructing the LedgerTxnEntry, as this
    // can throw and the LedgerTxnEntry destructor requires that mActive
    // contains key. LedgerTxnEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerTxnEntry ltxe(impl);

    // std::shared_ptr assignment is noexcept
    mEntry[key] = current;
    return ltxe;
}

std::map<AccountID, std::vector<LedgerTxnEntry>>
LedgerTxn::loadAllOffers()
{
    return getImpl()->loadAllOffers(*this);
}

std::map<AccountID, std::vector<LedgerTxnEntry>>
LedgerTxn::Impl::loadAllOffers(LedgerTxn& self)
{
    throwIfSealed();
    throwIfChild();

    auto previousEntries = mEntry;
    auto offers = getAllOffers();
    try
    {
        std::map<AccountID, std::vector<LedgerTxnEntry>> offersByAccount;
        for (auto const& kv : offers)
        {
            auto const& key = kv.first;
            auto const& sellerID = key.offer().sellerID;
            offersByAccount[sellerID].emplace_back(load(self, key));
        }
        return offersByAccount;
    }
    catch (...)
    {
        // For associative containers, swap does not throw unless the exception
        // is thrown by the swap of the Compare object (which is of type
        // std::less<LedgerKey>, so this should not throw when swapped)
        mEntry.swap(previousEntries);
        throw;
    }
}

LedgerTxnEntry
LedgerTxn::loadBestOffer(Asset const& buying, Asset const& selling)
{
    return getImpl()->loadBestOffer(*this, buying, selling);
}

LedgerTxnEntry
LedgerTxn::Impl::loadBestOffer(LedgerTxn& self, Asset const& buying,
                               Asset const& selling)
{
    throwIfSealed();
    throwIfChild();

    std::unordered_set<LedgerKey> exclude;
    auto le = getBestOffer(buying, selling, exclude);
    return le ? load(self, LedgerEntryKey(*le)) : LedgerTxnEntry();
}

LedgerTxnHeader
LedgerTxn::loadHeader()
{
    return getImpl()->loadHeader(*this);
}

LedgerTxnHeader
LedgerTxn::Impl::loadHeader(LedgerTxn& self)
{
    throwIfSealed();
    throwIfChild();
    if (mActiveHeader)
    {
        throw std::runtime_error("LedgerTxnHeader is active");
    }

    // Set the key to active before constructing the LedgerTxnHeader, as this
    // can throw and the LedgerTxnHeader destructor requires that
    // mActiveHeader is not empty. LedgerTxnHeader constructor does not throw
    // so this is still exception safe.
    mActiveHeader = LedgerTxnHeader::makeSharedImpl(self, *mHeader);
    return LedgerTxnHeader(mActiveHeader);
}

std::vector<LedgerTxnEntry>
LedgerTxn::loadOffersByAccountAndAsset(AccountID const& accountID,
                                       Asset const& asset)
{
    return getImpl()->loadOffersByAccountAndAsset(*this, accountID, asset);
}

std::vector<LedgerTxnEntry>
LedgerTxn::Impl::loadOffersByAccountAndAsset(LedgerTxn& self,
                                             AccountID const& accountID,
                                             Asset const& asset)
{
    throwIfSealed();
    throwIfChild();

    auto previousEntries = mEntry;
    auto offers = getOffersByAccountAndAsset(accountID, asset);
    try
    {
        std::vector<LedgerTxnEntry> res;
        res.reserve(offers.size());
        for (auto const& kv : offers)
        {
            auto const& key = kv.first;
            res.emplace_back(load(self, key));
        }
        return res;
    }
    catch (...)
    {
        // For associative containers, swap does not throw unless the exception
        // is thrown by the swap of the Compare object (which is of type
        // std::less<LedgerKey>, so this should not throw when swapped)
        mEntry.swap(previousEntries);
        throw;
    }
}

ConstLedgerTxnEntry
LedgerTxn::loadWithoutRecord(LedgerKey const& key)
{
    return getImpl()->loadWithoutRecord(*this, key);
}

ConstLedgerTxnEntry
LedgerTxn::Impl::loadWithoutRecord(LedgerTxn& self, LedgerKey const& key)
{
    throwIfSealed();
    throwIfChild();
    if (mActive.find(key) != mActive.end())
    {
        throw std::runtime_error("Key is active");
    }

    auto newest = getNewestVersion(key);
    if (!newest)
    {
        return {};
    }

    auto impl = ConstLedgerTxnEntry::makeSharedImpl(self, *newest);

    // Set the key to active before constructing the ConstLedgerTxnEntry, as
    // this can throw and the LedgerTxnEntry destructor requires that mActive
    // contains key. ConstLedgerTxnEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    return ConstLedgerTxnEntry(impl);
}

void
LedgerTxn::rollback()
{
    getImpl()->rollback();
    mImpl.reset();
}

void
LedgerTxn::Impl::rollback()
{
    if (mChild)
    {
        mChild->rollback();
    }

    mActive.clear();
    mActiveHeader.reset();

    mParent.rollbackChild();
}

void
LedgerTxn::rollbackChild()
{
    getImpl()->rollbackChild();
}

void
LedgerTxn::Impl::rollbackChild()
{
    mChild = nullptr;
}

void
LedgerTxn::unsealHeader(std::function<void(LedgerHeader&)> f)
{
    getImpl()->unsealHeader(*this, f);
}

void
LedgerTxn::Impl::unsealHeader(LedgerTxn& self,
                              std::function<void(LedgerHeader&)> f)
{
    if (!mIsSealed)
    {
        throw std::runtime_error("LedgerTxn is not sealed");
    }
    if (mActiveHeader)
    {
        throw std::runtime_error("LedgerTxnHeader is active");
    }

    mActiveHeader = LedgerTxnHeader::makeSharedImpl(self, *mHeader);
    LedgerTxnHeader header(mActiveHeader);
    f(header.current());
}

LedgerTxn::Impl::EntryMap
LedgerTxn::Impl::maybeUpdateLastModified() const
{
    throwIfSealed();
    throwIfChild();

    // Note: We do a deep copy here since a shallow copy would not be exception
    // safe.
    EntryMap entries;
    entries.reserve(mEntry.size());
    for (auto const& kv : mEntry)
    {
        auto const& key = kv.first;
        std::shared_ptr<LedgerEntry> entry;
        if (kv.second)
        {
            entry = std::make_shared<LedgerEntry>(*kv.second);
            if (mShouldUpdateLastModified)
            {
                entry->lastModifiedLedgerSeq = mHeader->ledgerSeq;
            }
        }
        entries.emplace(key, entry);
    }
    return entries;
}

void
LedgerTxn::Impl::maybeUpdateLastModifiedThenInvokeThenSeal(
    std::function<void(EntryMap const&)> f)
{
    if (!mIsSealed)
    {
        // Invokes throwIfChild and throwIfSealed
        auto entries = maybeUpdateLastModified();

        f(entries);

        // For associative containers, swap does not throw unless the exception
        // is thrown by the swap of the Compare object (which is of type
        // std::less<LedgerKey>, so this should not throw when swapped)
        mEntry.swap(entries);

        // std::set<...>::clear does not throw
        // std::shared_ptr<...>::reset does not throw
        mActive.clear();
        mActiveHeader.reset();
        mIsSealed = true;
    }
    else // Note: can't have child if sealed
    {
        f(mEntry);
    }
}

// Implementation of LedgerTxn::Impl::EntryIteratorImpl ---------------------
LedgerTxn::Impl::EntryIteratorImpl::EntryIteratorImpl(IteratorType const& begin,
                                                      IteratorType const& end)
    : mIter(begin), mEnd(end)
{
}

void
LedgerTxn::Impl::EntryIteratorImpl::advance()
{
    ++mIter;
}

bool
LedgerTxn::Impl::EntryIteratorImpl::atEnd() const
{
    return mIter == mEnd;
}

LedgerEntry const&
LedgerTxn::Impl::EntryIteratorImpl::entry() const
{
    return *(mIter->second);
}

bool
LedgerTxn::Impl::EntryIteratorImpl::entryExists() const
{
    return (bool)(mIter->second);
}

LedgerKey const&
LedgerTxn::Impl::EntryIteratorImpl::key() const
{
    return mIter->first;
}

// Implementation of LedgerTxnRoot ------------------------------------------
LedgerTxnRoot::LedgerTxnRoot(Database& db, size_t entryCacheSize,
                             size_t bestOfferCacheSize)
    : mImpl(std::make_unique<Impl>(db, entryCacheSize, bestOfferCacheSize))
{
}

LedgerTxnRoot::Impl::Impl(Database& db, size_t entryCacheSize,
                          size_t bestOfferCacheSize)
    : mDatabase(db)
    , mHeader(std::make_unique<LedgerHeader>())
    , mEntryCache(entryCacheSize)
    , mBestOffersCache(bestOfferCacheSize)
    , mChild(nullptr)
{
}

LedgerTxnRoot::~LedgerTxnRoot()
{
}

LedgerTxnRoot::Impl::~Impl()
{
    if (mChild)
    {
        mChild->rollback();
    }
}

void
LedgerTxnRoot::addChild(AbstractLedgerTxn& child)
{
    mImpl->addChild(child);
}

void
LedgerTxnRoot::Impl::addChild(AbstractLedgerTxn& child)
{
    if (mChild)
    {
        throw std::runtime_error("LedgerTxnRoot already has child");
    }
    mTransaction = std::make_unique<soci::transaction>(mDatabase.getSession());
    mChild = &child;
}

void
LedgerTxnRoot::Impl::throwIfChild() const
{
    if (mChild)
    {
        throw std::runtime_error("LedgerTxnRoot has child");
    }
}

void
LedgerTxnRoot::commitChild(EntryIterator iter)
{
    mImpl->commitChild(std::move(iter));
}

void
LedgerTxnRoot::Impl::commitChild(EntryIterator iter)
{
    // Assignment of xdrpp objects does not have the strong exception safety
    // guarantee, so use std::unique_ptr<...>::swap to achieve it
    auto childHeader = std::make_unique<LedgerHeader>(mChild->getHeader());

    try
    {
        for (; (bool)iter; ++iter)
        {
            auto const& key = iter.key();
            switch (key.type())
            {
            case ACCOUNT:
                storeAccount(iter);
                break;
            case DATA:
                storeData(iter);
                break;
            case OFFER:
                storeOffer(iter);
                break;
            case TRUSTLINE:
                storeTrustLine(iter);
                break;
            default:
                throw std::runtime_error("Unknown key type");
            }
        }

        mTransaction->commit();
        mDatabase.clearPreparedStatementCache();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error during commit to LedgerTxnRoot: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error during commit to LedgerTxnRoot");
    }

    // Clearing the cache does not throw
    mBestOffersCache.clear();
    mEntryCache.clear();

    // std::unique_ptr<...>::reset does not throw
    mTransaction.reset();

    // std::unique_ptr<...>::swap does not throw
    mHeader.swap(childHeader);
    mChild = nullptr;
}

std::string
LedgerTxnRoot::Impl::tableFromLedgerEntryType(LedgerEntryType let)
{
    switch (let)
    {
    case ACCOUNT:
        return "accounts";
    case DATA:
        return "accountdata";
    case OFFER:
        return "offers";
    case TRUSTLINE:
        return "trustlines";
    default:
        throw std::runtime_error("Unknown ledger entry type");
    }
}

uint64_t
LedgerTxnRoot::countObjects(LedgerEntryType let) const
{
    return mImpl->countObjects(let);
}

uint64_t
LedgerTxnRoot::Impl::countObjects(LedgerEntryType let) const
{
    using namespace soci;
    throwIfChild();

    std::string query =
        "SELECT COUNT(*) FROM " + tableFromLedgerEntryType(let) + ";";
    uint64_t count = 0;
    mDatabase.getSession() << query, into(count);
    return count;
}

uint64_t
LedgerTxnRoot::countObjects(LedgerEntryType let,
                            LedgerRange const& ledgers) const
{
    return mImpl->countObjects(let, ledgers);
}

uint64_t
LedgerTxnRoot::Impl::countObjects(LedgerEntryType let,
                                  LedgerRange const& ledgers) const
{
    using namespace soci;
    throwIfChild();

    std::string query = "SELECT COUNT(*) FROM " +
                        tableFromLedgerEntryType(let) +
                        " WHERE lastmodified >= :v1 AND lastmodified <= :v2;";
    uint64_t count = 0;
    int first = static_cast<int>(ledgers.first());
    int last = static_cast<int>(ledgers.last());
    mDatabase.getSession() << query, into(count), use(first), use(last);
    return count;
}

void
LedgerTxnRoot::deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const
{
    return mImpl->deleteObjectsModifiedOnOrAfterLedger(ledger);
}

void
LedgerTxnRoot::Impl::deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const
{
    using namespace soci;
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    {
        std::string query =
            "DELETE FROM signers WHERE accountid IN"
            " (SELECT accountid FROM accounts WHERE lastmodified >= :v1)";
        mDatabase.getSession() << query, use(ledger);
    }

    for (auto let : {ACCOUNT, DATA, TRUSTLINE, OFFER})
    {
        std::string query = "DELETE FROM " + tableFromLedgerEntryType(let) +
                            " WHERE lastmodified >= :v1";
        mDatabase.getSession() << query, use(ledger);
    }
}

void
LedgerTxnRoot::dropAccounts()
{
    mImpl->dropAccounts();
}

void
LedgerTxnRoot::dropData()
{
    mImpl->dropData();
}

void
LedgerTxnRoot::dropOffers()
{
    mImpl->dropOffers();
}

void
LedgerTxnRoot::dropTrustLines()
{
    mImpl->dropTrustLines();
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxnRoot::getAllOffers()
{
    return mImpl->getAllOffers();
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxnRoot::Impl::getAllOffers()
{
    std::vector<LedgerEntry> offers;
    try
    {
        offers = loadAllOffers();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when getting all offers from LedgerTxnRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error when getting all offers from LedgerTxnRoot");
    }

    std::unordered_map<LedgerKey, LedgerEntry> offersByKey(offers.size());
    for (auto const& offer : offers)
    {
        offersByKey.emplace(LedgerEntryKey(offer), offer);
    }
    return offersByKey;
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::getBestOffer(Asset const& buying, Asset const& selling,
                            std::unordered_set<LedgerKey>& exclude)
{
    return mImpl->getBestOffer(buying, selling, exclude);
}

static std::shared_ptr<LedgerEntry const>
findIncludedOffer(std::list<LedgerEntry>::const_iterator iter,
                  std::list<LedgerEntry>::const_iterator const& end,
                  std::unordered_set<LedgerKey> const& exclude)
{
    for (; iter != end; ++iter)
    {
        auto key = LedgerEntryKey(*iter);
        if (exclude.find(key) == exclude.end())
        {
            return std::make_shared<LedgerEntry const>(*iter);
        }
    }
    return {};
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::getBestOffer(Asset const& buying, Asset const& selling,
                                  std::unordered_set<LedgerKey>& exclude)
{
    // Note: Elements of mBestOffersCache are properly sorted lists of the best
    // offers for a certain asset pair. This function maintaints the invariant
    // that the lists of best offers remain properly sorted. The sort order is
    // that determined by loadBestOffers and isBetterOffer (both induce the same
    // order).
    BestOffersCacheEntry emptyCacheEntry{{}, false};
    auto& cached = getFromBestOffersCache(buying, selling, emptyCacheEntry);
    auto& offers = cached.bestOffers;

    auto res = findIncludedOffer(offers.cbegin(), offers.cend(), exclude);

    size_t const BATCH_SIZE = 5;
    while (!res && !cached.allLoaded)
    {
        std::list<LedgerEntry>::const_iterator newOfferIter;
        try
        {
            newOfferIter = loadBestOffers(offers, buying, selling, BATCH_SIZE,
                                          offers.size());
        }
        catch (std::exception& e)
        {
            printErrorAndAbort(
                "fatal error when getting best offer from LedgerTxnRoot: ",
                e.what());
        }
        catch (...)
        {
            printErrorAndAbort("unknown fatal error when getting best offer "
                               "from LedgerTxnRoot");
        }

        if (std::distance(newOfferIter, offers.cend()) < BATCH_SIZE)
        {
            cached.allLoaded = true;
        }
        res = findIncludedOffer(newOfferIter, offers.cend(), exclude);
    }
    return res;
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxnRoot::getOffersByAccountAndAsset(AccountID const& account,
                                          Asset const& asset)
{
    return mImpl->getOffersByAccountAndAsset(account, asset);
}

std::unordered_map<LedgerKey, LedgerEntry>
LedgerTxnRoot::Impl::getOffersByAccountAndAsset(AccountID const& account,
                                                Asset const& asset)
{
    std::vector<LedgerEntry> offers;
    try
    {
        offers = loadOffersByAccountAndAsset(account, asset);
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error when getting offers by account and "
                           "asset from LedgerTxnRoot: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when getting offers by account "
                           "and asset from LedgerTxnRoot");
    }

    std::unordered_map<LedgerKey, LedgerEntry> res(offers.size());
    for (auto const& offer : offers)
    {
        res.emplace(LedgerEntryKey(offer), offer);
    }
    return res;
}

LedgerHeader const&
LedgerTxnRoot::getHeader() const
{
    return mImpl->getHeader();
}

LedgerHeader const&
LedgerTxnRoot::Impl::getHeader() const
{
    return *mHeader;
}

std::vector<InflationWinner>
LedgerTxnRoot::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    return mImpl->getInflationWinners(maxWinners, minVotes);
}

std::vector<InflationWinner>
LedgerTxnRoot::Impl::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    try
    {
        return loadInflationWinners(maxWinners, minVotes);
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when getting inflation winners from LedgerTxnRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when getting inflation winners "
                           "from LedgerTxnRoot");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::getNewestVersion(LedgerKey const& key) const
{
    return mImpl->getNewestVersion(key);
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::getNewestVersion(LedgerKey const& key) const
{
    auto cacheKey = getEntryCacheKey(key);
    if (mEntryCache.exists(cacheKey))
    {
        return getFromEntryCache(cacheKey);
    }

    std::shared_ptr<LedgerEntry const> entry;
    try
    {
        switch (key.type())
        {
        case ACCOUNT:
            entry = loadAccount(key);
            break;
        case DATA:
            entry = loadData(key);
            break;
        case OFFER:
            entry = loadOffer(key);
            break;
        case TRUSTLINE:
            entry = loadTrustLine(key);
            break;
        default:
            throw std::runtime_error("Unknown key type");
        }
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when loading ledger entry from LedgerTxnRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when loading ledger entry from "
                           "LedgerTxnRoot");
    }

    putInEntryCache(cacheKey, entry);
    return entry;
}

void
LedgerTxnRoot::rollbackChild()
{
    mImpl->rollbackChild();
}

void
LedgerTxnRoot::Impl::rollbackChild()
{
    try
    {
        mTransaction->rollback();
        mTransaction.reset();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when rolling back child of LedgerTxnRoot: ", e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error when rolling back child of LedgerTxnRoot");
    }

    mChild = nullptr;
}

void
LedgerTxnRoot::Impl::storeAccount(EntryIterator const& iter)
{
    if (iter.entryExists())
    {
        auto const previous = getNewestVersion(iter.key());
        insertOrUpdateAccount(iter.entry(), !previous);
        storeSigners(iter.entry(), previous);
    }
    else
    {
        deleteAccount(iter.key());
    }
}

void
LedgerTxnRoot::Impl::storeData(EntryIterator const& iter)
{
    if (iter.entryExists())
    {
        auto const previous = getNewestVersion(iter.key());
        insertOrUpdateData(iter.entry(), !previous);
    }
    else
    {
        deleteData(iter.key());
    }
}

void
LedgerTxnRoot::Impl::storeOffer(EntryIterator const& iter)
{
    if (iter.entryExists())
    {
        auto const previous = getNewestVersion(iter.key());
        insertOrUpdateOffer(iter.entry(), !previous);
    }
    else
    {
        deleteOffer(iter.key());
    }
}

void
LedgerTxnRoot::Impl::storeTrustLine(EntryIterator const& iter)
{
    if (iter.entryExists())
    {
        auto const previous = getNewestVersion(iter.key());
        insertOrUpdateTrustLine(iter.entry(), !previous);
    }
    else
    {
        deleteTrustLine(iter.key());
    }
}

LedgerTxnRoot::Impl::EntryCacheKey
LedgerTxnRoot::Impl::getEntryCacheKey(LedgerKey const& key) const
{
    return binToHex(xdr::xdr_to_opaque(key));
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::getFromEntryCache(EntryCacheKey const& cacheKey) const
{
    try
    {
        return mEntryCache.get(cacheKey);
    }
    catch (...)
    {
        mEntryCache.clear();
        throw;
    }
}

void
LedgerTxnRoot::Impl::putInEntryCache(
    EntryCacheKey const& cacheKey,
    std::shared_ptr<LedgerEntry const> const& entry) const
{
    try
    {
        mEntryCache.put(cacheKey, entry);
    }
    catch (...)
    {
        mEntryCache.clear();
        throw;
    }
}

LedgerTxnRoot::Impl::BestOffersCacheEntry&
LedgerTxnRoot::Impl::getFromBestOffersCache(
    Asset const& buying, Asset const& selling,
    BestOffersCacheEntry& defaultValue) const
{
    try
    {
        auto cacheKey = binToHex(xdr::xdr_to_opaque(buying)) +
                        binToHex(xdr::xdr_to_opaque(selling));
        if (!mBestOffersCache.exists(cacheKey))
        {
            mBestOffersCache.put(cacheKey, defaultValue);
        }
        return mBestOffersCache.exists(cacheKey)
                   ? mBestOffersCache.get(cacheKey)
                   : defaultValue;
    }
    catch (...)
    {
        mBestOffersCache.clear();
        throw;
    }
}
}
