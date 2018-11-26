// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerState.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "ledger/LedgerStateImpl.h"
#include "util/GlobalChecks.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/marshal.h"
#include <soci.h>

namespace stellar
{

// Implementation of AbstractLedgerStateParent --------------------------------
AbstractLedgerStateParent::~AbstractLedgerStateParent()
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

// Implementation of AbstractLedgerState --------------------------------------
AbstractLedgerState::~AbstractLedgerState()
{
}

// Implementation of LedgerState ----------------------------------------------
LedgerState::LedgerState(AbstractLedgerStateParent& parent,
                         bool shouldUpdateLastModified)
    : mImpl(std::make_unique<Impl>(*this, parent, shouldUpdateLastModified))
{
}

LedgerState::LedgerState(LedgerState& parent, bool shouldUpdateLastModified)
    : LedgerState((AbstractLedgerStateParent&)parent, shouldUpdateLastModified)
{
}

LedgerState::Impl::Impl(LedgerState& self, AbstractLedgerStateParent& parent,
                        bool shouldUpdateLastModified)
    : mParent(parent)
    , mChild(nullptr)
    , mHeader(std::make_unique<LedgerHeader>(mParent.getHeader()))
    , mShouldUpdateLastModified(shouldUpdateLastModified)
    , mIsSealed(false)
{
    mParent.addChild(self);
}

LedgerState::~LedgerState()
{
    if (mImpl)
    {
        rollback();
    }
}

std::unique_ptr<LedgerState::Impl> const&
LedgerState::getImpl() const
{
    if (!mImpl)
    {
        throw std::runtime_error("LedgerStateEntry was handled");
    }
    return mImpl;
}

void
LedgerState::addChild(AbstractLedgerState& child)
{
    getImpl()->addChild(child);
}

void
LedgerState::Impl::addChild(AbstractLedgerState& child)
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
LedgerState::Impl::throwIfChild() const
{
    if (mChild)
    {
        throw std::runtime_error("LedgerState has child");
    }
}

void
LedgerState::Impl::throwIfSealed() const
{
    if (mIsSealed)
    {
        throw std::runtime_error("LedgerState is sealed");
    }
}

void
LedgerState::commit()
{
    getImpl()->commit();
    mImpl.reset();
}

void
LedgerState::Impl::commit()
{
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        // getEntryIterator has the strong exception safety guarantee
        // commitChild has the strong exception safety guarantee
        mParent.commitChild(getEntryIterator(entries));
    });
}

void
LedgerState::commitChild(EntryIterator iter)
{
    getImpl()->commitChild(std::move(iter));
}

void
LedgerState::Impl::commitChild(EntryIterator iter)
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
            { // Created in this LedgerState
                mEntry.erase(key);
            }
            else
            { // Existed in a previous LedgerState
                mEntry[key] = nullptr;
            }
        }
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error during commit to LedgerState: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error during commit to LedgerState");
    }

    // std::unique_ptr<...>::swap does not throw
    mHeader.swap(childHeader);
    mChild = nullptr;
}

LedgerStateEntry
LedgerState::create(LedgerEntry const& entry)
{
    return getImpl()->create(*this, entry);
}

LedgerStateEntry
LedgerState::Impl::create(LedgerState& self, LedgerEntry const& entry)
{
    throwIfSealed();
    throwIfChild();

    auto key = LedgerEntryKey(entry);
    if (getNewestVersion(key))
    {
        throw std::runtime_error("Key already exists");
    }

    auto current = std::make_shared<LedgerEntry>(entry);
    auto impl = LedgerStateEntry::makeSharedImpl(self, *current);

    // Set the key to active before constructing the LedgerStateEntry, as this
    // can throw and the LedgerStateEntry destructor requires that mActive
    // contains key. LedgerStateEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerStateEntry lse(impl);

    // std::shared_ptr assignment is noexcept
    mEntry[key] = current;
    return lse;
}

void
LedgerState::deactivate(LedgerKey const& key)
{
    getImpl()->deactivate(key);
}

void
LedgerState::Impl::deactivate(LedgerKey const& key)
{
    auto iter = mActive.find(key);
    if (iter == mActive.end())
    {
        throw std::runtime_error("Key is not active");
    }
    mActive.erase(iter);
}

void
LedgerState::deactivateHeader()
{
    getImpl()->deactivateHeader();
}

void
LedgerState::Impl::deactivateHeader()
{
    if (!mActiveHeader)
    {
        throw std::runtime_error("LedgerStateHeader is not active");
    }
    mActiveHeader.reset();
}

void
LedgerState::erase(LedgerKey const& key)
{
    getImpl()->erase(key);
}

void
LedgerState::Impl::erase(LedgerKey const& key)
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
    { // Created in this LedgerState
        mEntry.erase(key);
    }
    else
    { // Existed in a previous LedgerState
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

std::map<LedgerKey, LedgerEntry>
LedgerState::getAllOffers()
{
    return getImpl()->getAllOffers();
}

std::map<LedgerKey, LedgerEntry>
LedgerState::Impl::getAllOffers()
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
LedgerState::getBestOffer(Asset const& buying, Asset const& selling,
                          std::set<LedgerKey>& exclude)
{
    return getImpl()->getBestOffer(buying, selling, exclude);
}

std::shared_ptr<LedgerEntry const>
LedgerState::Impl::getBestOffer(Asset const& buying, Asset const& selling,
                                std::set<LedgerKey>& exclude)
{
    auto end = mEntry.cend();
    auto bestOfferIter = end;
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
LedgerState::getChanges()
{
    return getImpl()->getChanges();
}

LedgerEntryChanges
LedgerState::Impl::getChanges()
{
    LedgerEntryChanges changes;
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
                // erased in this LedgerState, in which case it should not still
                // be in this LedgerState
                assert(entry);
                changes.emplace_back(LEDGER_ENTRY_CREATED);
                changes.back().created() = *entry;
            }
        }
    });
    return changes;
}

std::vector<LedgerKey>
LedgerState::getDeadEntries()
{
    return getImpl()->getDeadEntries();
}

std::vector<LedgerKey>
LedgerState::Impl::getDeadEntries()
{
    std::vector<LedgerKey> res;
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

LedgerStateDelta
LedgerState::getDelta()
{
    return getImpl()->getDelta();
}

LedgerStateDelta
LedgerState::Impl::getDelta()
{
    LedgerStateDelta delta;
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& key = kv.first;
            auto previous = mParent.getNewestVersion(key);

            // Deep copy is not required here because getDelta causes
            // LedgerState to enter the sealed state, meaning subsequent
            // modifications are impossible.
            delta.entry[key] = {kv.second, previous};
        }
        delta.header = {*mHeader, mParent.getHeader()};
    });
    return delta;
}

EntryIterator
LedgerState::Impl::getEntryIterator(EntryMap const& entries) const
{
    auto iterImpl =
        std::make_unique<EntryIteratorImpl>(entries.cbegin(), entries.cend());
    return EntryIterator(std::move(iterImpl));
}

LedgerHeader const&
LedgerState::getHeader() const
{
    return getImpl()->getHeader();
}

LedgerHeader const&
LedgerState::Impl::getHeader() const
{
    return *mHeader;
}

std::vector<InflationWinner>
LedgerState::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    return getImpl()->getInflationWinners(maxWinners, minVotes);
}

std::map<AccountID, int64_t>
LedgerState::Impl::getDeltaVotes() const
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
LedgerState::Impl::getTotalVotes(
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
LedgerState::Impl::enumerateInflationWinners(
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
LedgerState::Impl::getInflationWinners(size_t maxWinners, int64_t minVotes)
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
LedgerState::queryInflationWinners(size_t maxWinners, int64_t minVotes)
{
    return getImpl()->queryInflationWinners(maxWinners, minVotes);
}

std::vector<InflationWinner>
LedgerState::Impl::queryInflationWinners(size_t maxWinners, int64_t minVotes)
{
    throwIfSealed();
    throwIfChild();
    return getInflationWinners(maxWinners, minVotes);
}

std::vector<LedgerEntry>
LedgerState::getLiveEntries()
{
    return getImpl()->getLiveEntries();
}

std::vector<LedgerEntry>
LedgerState::Impl::getLiveEntries()
{
    std::vector<LedgerEntry> res;
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
LedgerState::getNewestVersion(LedgerKey const& key) const
{
    return getImpl()->getNewestVersion(key);
}

std::shared_ptr<LedgerEntry const>
LedgerState::Impl::getNewestVersion(LedgerKey const& key) const
{
    auto iter = mEntry.find(key);
    if (iter != mEntry.end())
    {
        return iter->second;
    }
    return mParent.getNewestVersion(key);
}

std::map<LedgerKey, LedgerEntry>
LedgerState::getOffersByAccountAndAsset(AccountID const& account,
                                        Asset const& asset)
{
    return getImpl()->getOffersByAccountAndAsset(account, asset);
}

std::map<LedgerKey, LedgerEntry>
LedgerState::Impl::getOffersByAccountAndAsset(AccountID const& account,
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

LedgerStateEntry
LedgerState::load(LedgerKey const& key)
{
    return getImpl()->load(*this, key);
}

LedgerStateEntry
LedgerState::Impl::load(LedgerState& self, LedgerKey const& key)
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
    auto impl = LedgerStateEntry::makeSharedImpl(self, *current);

    // Set the key to active before constructing the LedgerStateEntry, as this
    // can throw and the LedgerStateEntry destructor requires that mActive
    // contains key. LedgerStateEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerStateEntry lse(impl);

    // std::shared_ptr assignment is noexcept
    mEntry[key] = current;
    return lse;
}

std::map<AccountID, std::vector<LedgerStateEntry>>
LedgerState::loadAllOffers()
{
    return getImpl()->loadAllOffers(*this);
}

std::map<AccountID, std::vector<LedgerStateEntry>>
LedgerState::Impl::loadAllOffers(LedgerState& self)
{
    throwIfSealed();
    throwIfChild();

    auto previousEntries = mEntry;
    auto offers = getAllOffers();
    try
    {
        std::map<AccountID, std::vector<LedgerStateEntry>> offersByAccount;
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

LedgerStateEntry
LedgerState::loadBestOffer(Asset const& buying, Asset const& selling)
{
    return getImpl()->loadBestOffer(*this, buying, selling);
}

LedgerStateEntry
LedgerState::Impl::loadBestOffer(LedgerState& self, Asset const& buying,
                                 Asset const& selling)
{
    throwIfSealed();
    throwIfChild();

    std::set<LedgerKey> exclude;
    auto le = getBestOffer(buying, selling, exclude);
    return le ? load(self, LedgerEntryKey(*le)) : LedgerStateEntry();
}

LedgerStateHeader
LedgerState::loadHeader()
{
    return getImpl()->loadHeader(*this);
}

LedgerStateHeader
LedgerState::Impl::loadHeader(LedgerState& self)
{
    throwIfSealed();
    throwIfChild();
    if (mActiveHeader)
    {
        throw std::runtime_error("LedgerStateHeader is active");
    }

    // Set the key to active before constructing the LedgerStateHeader, as this
    // can throw and the LedgerStateHeader destructor requires that
    // mActiveHeader is not empty. LedgerStateHeader constructor does not throw
    // so this is still exception safe.
    mActiveHeader = LedgerStateHeader::makeSharedImpl(self, *mHeader);
    return LedgerStateHeader(mActiveHeader);
}

std::vector<LedgerStateEntry>
LedgerState::loadOffersByAccountAndAsset(AccountID const& accountID,
                                         Asset const& asset)
{
    return getImpl()->loadOffersByAccountAndAsset(*this, accountID, asset);
}

std::vector<LedgerStateEntry>
LedgerState::Impl::loadOffersByAccountAndAsset(LedgerState& self,
                                               AccountID const& accountID,
                                               Asset const& asset)
{
    throwIfSealed();
    throwIfChild();

    auto previousEntries = mEntry;
    auto offers = getOffersByAccountAndAsset(accountID, asset);
    try
    {
        std::vector<LedgerStateEntry> res;
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

ConstLedgerStateEntry
LedgerState::loadWithoutRecord(LedgerKey const& key)
{
    return getImpl()->loadWithoutRecord(*this, key);
}

ConstLedgerStateEntry
LedgerState::Impl::loadWithoutRecord(LedgerState& self, LedgerKey const& key)
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

    auto impl = ConstLedgerStateEntry::makeSharedImpl(self, *newest);

    // Set the key to active before constructing the ConstLedgerStateEntry, as
    // this can throw and the LedgerStateEntry destructor requires that mActive
    // contains key. ConstLedgerStateEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    return ConstLedgerStateEntry(impl);
}

void
LedgerState::rollback()
{
    getImpl()->rollback();
    mImpl.reset();
}

void
LedgerState::Impl::rollback()
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
LedgerState::rollbackChild()
{
    getImpl()->rollbackChild();
}

void
LedgerState::Impl::rollbackChild()
{
    mChild = nullptr;
}

void
LedgerState::unsealHeader(std::function<void(LedgerHeader&)> f)
{
    getImpl()->unsealHeader(*this, f);
}

void
LedgerState::Impl::unsealHeader(LedgerState& self,
                                std::function<void(LedgerHeader&)> f)
{
    if (!mIsSealed)
    {
        throw std::runtime_error("LedgerState is not sealed");
    }
    if (mActiveHeader)
    {
        throw std::runtime_error("LedgerStateHeader is active");
    }

    mActiveHeader = LedgerStateHeader::makeSharedImpl(self, *mHeader);
    LedgerStateHeader header(mActiveHeader);
    f(header.current());
}

LedgerState::Impl::EntryMap
LedgerState::Impl::maybeUpdateLastModified() const
{
    throwIfSealed();
    throwIfChild();

    // Note: We do a deep copy here since a shallow copy would not be exception
    // safe.
    EntryMap entries;
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
LedgerState::Impl::maybeUpdateLastModifiedThenInvokeThenSeal(
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

// Implementation of LedgerState::Impl::EntryIteratorImpl ---------------------
LedgerState::Impl::EntryIteratorImpl::EntryIteratorImpl(
    IteratorType const& begin, IteratorType const& end)
    : mIter(begin), mEnd(end)
{
}

void
LedgerState::Impl::EntryIteratorImpl::advance()
{
    ++mIter;
}

bool
LedgerState::Impl::EntryIteratorImpl::atEnd() const
{
    return mIter == mEnd;
}

LedgerEntry const&
LedgerState::Impl::EntryIteratorImpl::entry() const
{
    return *(mIter->second);
}

bool
LedgerState::Impl::EntryIteratorImpl::entryExists() const
{
    return (bool)(mIter->second);
}

LedgerKey const&
LedgerState::Impl::EntryIteratorImpl::key() const
{
    return mIter->first;
}

// Implementation of LedgerStateRoot ------------------------------------------
LedgerStateRoot::LedgerStateRoot(Database& db, size_t entryCacheSize,
                                 size_t bestOfferCacheSize)
    : mImpl(std::make_unique<Impl>(db, entryCacheSize, bestOfferCacheSize))
{
}

LedgerStateRoot::Impl::Impl(Database& db, size_t entryCacheSize,
                            size_t bestOfferCacheSize)
    : mDatabase(db)
    , mHeader(std::make_unique<LedgerHeader>())
    , mEntryCache(entryCacheSize)
    , mBestOffersCache(bestOfferCacheSize)
    , mChild(nullptr)
{
}

LedgerStateRoot::~LedgerStateRoot()
{
}

LedgerStateRoot::Impl::~Impl()
{
    if (mChild)
    {
        mChild->rollback();
    }
}

void
LedgerStateRoot::addChild(AbstractLedgerState& child)
{
    mImpl->addChild(child);
}

void
LedgerStateRoot::Impl::addChild(AbstractLedgerState& child)
{
    if (mChild)
    {
        throw std::runtime_error("LedgerStateRoot already has child");
    }
    mTransaction = std::make_unique<soci::transaction>(mDatabase.getSession());
    mChild = &child;
}

void
LedgerStateRoot::Impl::throwIfChild() const
{
    if (mChild)
    {
        throw std::runtime_error("LedgerStateRoot has child");
    }
}

void
LedgerStateRoot::commitChild(EntryIterator iter)
{
    mImpl->commitChild(std::move(iter));
}

void
LedgerStateRoot::Impl::commitChild(EntryIterator iter)
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
        printErrorAndAbort("fatal error during commit to LedgerStateRoot: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error during commit to LedgerStateRoot");
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
LedgerStateRoot::Impl::tableFromLedgerEntryType(LedgerEntryType let)
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
LedgerStateRoot::countObjects(LedgerEntryType let) const
{
    return mImpl->countObjects(let);
}

uint64_t
LedgerStateRoot::Impl::countObjects(LedgerEntryType let) const
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
LedgerStateRoot::countObjects(LedgerEntryType let,
                              LedgerRange const& ledgers) const
{
    return mImpl->countObjects(let, ledgers);
}

uint64_t
LedgerStateRoot::Impl::countObjects(LedgerEntryType let,
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
LedgerStateRoot::deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const
{
    return mImpl->deleteObjectsModifiedOnOrAfterLedger(ledger);
}

void
LedgerStateRoot::Impl::deleteObjectsModifiedOnOrAfterLedger(
    uint32_t ledger) const
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
LedgerStateRoot::dropAccounts()
{
    mImpl->dropAccounts();
}

void
LedgerStateRoot::dropData()
{
    mImpl->dropData();
}

void
LedgerStateRoot::dropOffers()
{
    mImpl->dropOffers();
}

void
LedgerStateRoot::dropTrustLines()
{
    mImpl->dropTrustLines();
}

std::map<LedgerKey, LedgerEntry>
LedgerStateRoot::getAllOffers()
{
    return mImpl->getAllOffers();
}

std::map<LedgerKey, LedgerEntry>
LedgerStateRoot::Impl::getAllOffers()
{
    std::vector<LedgerEntry> offers;
    try
    {
        offers = loadAllOffers();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when getting all offers from LedgerStateRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error when getting all offers from LedgerStateRoot");
    }

    std::map<LedgerKey, LedgerEntry> offersByKey;
    for (auto const& offer : offers)
    {
        offersByKey.emplace(LedgerEntryKey(offer), offer);
    }
    return offersByKey;
}

std::shared_ptr<LedgerEntry const>
LedgerStateRoot::getBestOffer(Asset const& buying, Asset const& selling,
                              std::set<LedgerKey>& exclude)
{
    return mImpl->getBestOffer(buying, selling, exclude);
}

static std::shared_ptr<LedgerEntry const>
findIncludedOffer(std::list<LedgerEntry>::const_iterator iter,
                  std::list<LedgerEntry>::const_iterator const& end,
                  std::set<LedgerKey> const& exclude)
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
LedgerStateRoot::Impl::getBestOffer(Asset const& buying, Asset const& selling,
                                    std::set<LedgerKey>& exclude)
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
                "fatal error when getting best offer from LedgerStateRoot: ",
                e.what());
        }
        catch (...)
        {
            printErrorAndAbort("unknown fatal error when getting best offer "
                               "from LedgerStateRoot");
        }

        if (std::distance(newOfferIter, offers.cend()) < BATCH_SIZE)
        {
            cached.allLoaded = true;
        }
        res = findIncludedOffer(newOfferIter, offers.cend(), exclude);
    }
    return res;
}

std::map<LedgerKey, LedgerEntry>
LedgerStateRoot::getOffersByAccountAndAsset(AccountID const& account,
                                            Asset const& asset)
{
    return mImpl->getOffersByAccountAndAsset(account, asset);
}

std::map<LedgerKey, LedgerEntry>
LedgerStateRoot::Impl::getOffersByAccountAndAsset(AccountID const& account,
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
                           "asset from LedgerStateRoot: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when getting offers by account "
                           "and asset from LedgerStateRoot");
    }

    std::map<LedgerKey, LedgerEntry> res;
    for (auto const& offer : offers)
    {
        res.emplace(LedgerEntryKey(offer), offer);
    }
    return res;
}

LedgerHeader const&
LedgerStateRoot::getHeader() const
{
    return mImpl->getHeader();
}

LedgerHeader const&
LedgerStateRoot::Impl::getHeader() const
{
    return *mHeader;
}

std::vector<InflationWinner>
LedgerStateRoot::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    return mImpl->getInflationWinners(maxWinners, minVotes);
}

std::vector<InflationWinner>
LedgerStateRoot::Impl::getInflationWinners(size_t maxWinners, int64_t minVotes)
{
    try
    {
        return loadInflationWinners(maxWinners, minVotes);
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when getting inflation winners from LedgerStateRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when getting inflation winners "
                           "from LedgerStateRoot");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerStateRoot::getNewestVersion(LedgerKey const& key) const
{
    return mImpl->getNewestVersion(key);
}

std::shared_ptr<LedgerEntry const>
LedgerStateRoot::Impl::getNewestVersion(LedgerKey const& key) const
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
            "fatal error when loading ledger entry from LedgerStateRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when loading ledger entry from "
                           "LedgerStateRoot");
    }

    putInEntryCache(cacheKey, entry);
    return entry;
}

void
LedgerStateRoot::rollbackChild()
{
    mImpl->rollbackChild();
}

void
LedgerStateRoot::Impl::rollbackChild()
{
    try
    {
        mTransaction->rollback();
        mTransaction.reset();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when rolling back child of LedgerStateRoot: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error when rolling back child of LedgerStateRoot");
    }

    mChild = nullptr;
}

void
LedgerStateRoot::Impl::storeAccount(EntryIterator const& iter)
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
LedgerStateRoot::Impl::storeData(EntryIterator const& iter)
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
LedgerStateRoot::Impl::storeOffer(EntryIterator const& iter)
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
LedgerStateRoot::Impl::storeTrustLine(EntryIterator const& iter)
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

LedgerStateRoot::Impl::EntryCacheKey
LedgerStateRoot::Impl::getEntryCacheKey(LedgerKey const& key) const
{
    return binToHex(xdr::xdr_to_opaque(key));
}

std::shared_ptr<LedgerEntry const>
LedgerStateRoot::Impl::getFromEntryCache(EntryCacheKey const& cacheKey) const
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
LedgerStateRoot::Impl::putInEntryCache(
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

LedgerStateRoot::Impl::BestOffersCacheEntry&
LedgerStateRoot::Impl::getFromBestOffersCache(
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
