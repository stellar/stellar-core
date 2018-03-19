// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerState.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "util/make_unique.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/marshal.h"

namespace stellar
{
using xdr::operator<;
using xdr::operator==;
using xdr::operator!=;

bool
operator==(InflationVotes const& lhs, InflationVotes const& rhs)
{
    return (lhs.votes == rhs.votes) && (lhs.inflationDest == rhs.inflationDest);
}

LedgerStateRoot::LedgerStateRoot(Database& db)
    : mHasChild(false), mDb(db) , mEntryCache(4096)
{
}

void
LedgerStateRoot::flushCache()
{
    getCache().clear();
}

bool
LedgerStateRoot::hasChild()
{
    return mHasChild;
}

void
LedgerStateRoot::hasChild(bool child)
{
    assert(!child || !mHasChild);
    mHasChild = child;
}

LedgerHeader const&
LedgerStateRoot::getCurrentHeader()
{
    return mCurrentHeader;
}

void
LedgerStateRoot::setCurrentHeader(LedgerHeader const& header)
{
    mCurrentHeader = header;
}

Database&
LedgerStateRoot::getDatabase()
{
    return mDb;
}

LedgerStateRoot::EntryCache&
LedgerStateRoot::getCache()
{
    return mEntryCache;
}

LedgerState::LedgerState(LedgerStateRoot& root)
    : mTransaction(make_unique<soci::transaction>(root.getDatabase().getSession()))
    , mRoot(&root), mParent(nullptr), mChild(nullptr)
{
    mRoot->hasChild(true);
}

LedgerState::LedgerState(LedgerState& parent)
    : mRoot(nullptr), mParent(&parent), mChild(nullptr)
    , mLoadBestOfferContext(mParent->mLoadBestOfferContext
            ? std::make_shared<LoadBestOfferContext>(
                std::move(*mParent->mLoadBestOfferContext), *this)
            : nullptr)
{
    assert(!mParent->mChild);
    mParent->mChild = this;
    for (auto const& state : mParent->mState)
    {
        state.second->invalidate();
    }
    if (mParent->mHeader)
    {
        mParent->mHeader->invalidate();
    }
    mParent->mLoadBestOfferContext.reset();
}

LedgerState::~LedgerState()
{
    // We want to rollback by default if neither commit nor rollback have been
    // called, and do nothing otherwise. When commit or rollback succeed, we
    // have mParent == mRoot == nullptr. Analogous asserts appear in commit and
    // rollback to prevent them from being called more than once for a given
    // LedgerState.
    if (mParent || mRoot)
    {
        rollback();
    }
}

LedgerEntryChanges
LedgerState::getChanges()
{
    assert(mParent || mRoot);
    assert(!mChild);

    LedgerEntryChanges changes;
    for (auto const& state : mState)
    {
        auto const& entry = state.second->ignoreInvalid().entry();
        auto const& previous = state.second->ignoreInvalid().previousEntry();
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
                changes.back().removed() = state.first;
            }
        }
        else
        {
            if (entry)
            {
                changes.emplace_back(LEDGER_ENTRY_CREATED);
                changes.back().created() = *entry;
            }
        }
    }
    return changes;
}

std::vector<LedgerEntry>
LedgerState::getLiveEntries()
{
    assert(mRoot);
    assert(!mChild);

    std::vector<LedgerEntry> entries;
    for (auto const& state : mState)
    {
        auto le = state.second->ignoreInvalid().entry();
        if (le)
        {
            entries.push_back(*le);
        }
    }
    return entries;
}

std::vector<LedgerKey>
LedgerState::getDeadEntries()
{
    assert(mRoot);
    assert(!mChild);

    std::vector<LedgerKey> keys;
    for (auto const& state : mState)
    {
        if (!state.second->ignoreInvalid().entry() &&
            state.second->ignoreInvalid().previousEntry())
        {
            keys.push_back(state.first);
        }
    }
    return keys;
}

void
LedgerState::commit(std::function<void()> onCommitToDatabase)
{
    assert(mParent || mRoot);
    assert(!mChild);

    if (mParent)
    {
        assert(!onCommitToDatabase);
        mergeStateIntoParent();
        mergeHeaderIntoParent();

        if (mLoadBestOfferContext)
        {
            mParent->mLoadBestOfferContext =
                std::make_shared<LoadBestOfferContext>(
                        std::move(*mLoadBestOfferContext), *mParent);
        }

        mParent->mChild = nullptr;
        mParent = nullptr;
    }
    else
    {
        try
        {
            mergeStateIntoRoot();
            mergeHeaderIntoRoot();
            if (onCommitToDatabase)
            {
                onCommitToDatabase();
            }
            mTransaction->commit();
        }
        catch (...)
        {
            mRoot->getCache().clear();
            throw;
        }

        mRoot->hasChild(false);
        mRoot = nullptr;
    }

    mHeader.reset();
    mState.clear();
    mLoadBestOfferContext.reset();
}

void
LedgerState::mergeStateIntoParent()
{
    for (auto const& state : mState)
    {
        state.second->invalidate();
        auto const& entry = state.second->ignoreInvalid().entry();

        auto iter = mParent->mState.find(state.first);
        if (iter == mParent->mState.end())
        {
            auto const& previous =
                state.second->ignoreInvalid().previousEntry();
            auto ler = makeStateEntry(entry, previous);
            ler->invalidate();
            mParent->mState[state.first] = ler;
        }
        else
        {
            auto const& previous =
                iter->second->ignoreInvalid().previousEntry();
            auto ler = makeStateEntry(entry, previous);
            ler->invalidate();
            iter->second = ler;
        }
    }
}

void
LedgerState::mergeHeaderIntoParent()
{
    if (mHeader)
    {
        auto const& header = mHeader->ignoreInvalid().header();
        auto const& previous = mHeader->ignoreInvalid().previousHeader();
        auto lhr = makeStateHeader(header, previous);
        lhr->invalidate();
        mParent->mHeader = lhr;
    }
}

void
LedgerState::mergeStateIntoRoot()
{
    for (auto const& state : mState)
    {
        state.second->invalidate();
        if (state.second->ignoreInvalid().entry())
        {
            storeInDatabase(state.second);
        }
        else
        {
            deleteFromDatabase(state.second);
        }
    }
}

void
LedgerState::mergeHeaderIntoRoot()
{
    if (!mHeader)
    {
        loadHeader()->invalidate();
    }
    mRoot->setCurrentHeader(mHeader->ignoreInvalid().header());
}

void
LedgerState::rollback()
{
    assert(mParent || mRoot);

    if (mChild)
    {
        mChild->rollback();
    }

    if (mParent)
    {
        mParent->mChild = nullptr;
        mParent = nullptr;
    }
    else
    {
        mRoot->hasChild(false);
        mRoot = nullptr;
        mTransaction->rollback();
    }

    mHeader.reset();
    mState.clear();
    mLoadBestOfferContext.reset();
}

LedgerState::StateEntry
LedgerState::create(LedgerEntry const& entry)
{
    assert(mParent || mRoot);
    assert(!mChild);

    auto key = LedgerEntryKey(entry);
    auto ler = createHelper(entry, key);
    mState[key] = ler;

    if (entry.data.type() == OFFER)
    {
        OfferEntry const& offer = entry.data.offer();
        invalidateLoadBestOfferContext(offer.selling, offer.buying);
    }

    return ler;
}

LedgerState::StateEntry
LedgerState::createHelper(LedgerEntry const& entry, LedgerKey const& key)
{
    StateEntry ler;

    auto iter = mState.find(key);
    if (iter != mState.end())
    {
        // This condition handles cases like the following example:
        //    LedgerKey key = ...;
        //    LedgerState lsParent(...);
        //    LedgerState lsChild(lsParent);
        //    lsChild.load(key)->erase();
        //    lsChild.commit();
        //    lsParent.create(key);
        if (iter->second->ignoreInvalid().entry())
        {
            throw std::runtime_error("Key already exists in memory");
        }

        auto newEntry = std::make_shared<LedgerEntry>(entry);
        if (!mChild)
        {
            auto const& previous =
                iter->second->ignoreInvalid().previousEntry();
            ler = makeStateEntry(newEntry, previous);
        }
        else
        {
            ler = makeStateEntry(newEntry, nullptr);
        }
    }
    else if (mParent)
    {
        ler = mParent->createHelper(entry, key);
    }
    else
    {
        if (loadFromDatabase(key))
        {
            throw std::runtime_error("Key already exists in database");
        }
        auto newEntry = std::make_shared<LedgerEntry>(entry);
        ler = makeStateEntry(newEntry, nullptr);
    }
    return ler;
}

LedgerState::StateEntry
LedgerState::load(LedgerKey const& key)
{
    assert(mParent || mRoot);
    assert(!mChild);

    auto ler = loadHelper(key);
    mState[key] = ler;

    if (ler->entry()->data.type() == OFFER)
    {
        OfferEntry const& offer = ler->entry()->data.offer();
        invalidateLoadBestOfferContext(offer.selling, offer.buying);
    }

    return ler;
}

LedgerState::StateEntry
LedgerState::loadHelper(LedgerKey const& key)
{
    StateEntry ler;

    auto iter = mState.find(key);
    if (iter != mState.end())
    {
        // This condition handles cases like the following example:
        //    LedgerKey key = ...;
        //    LedgerState lsParent(...);
        //    LedgerState lsChild(lsParent);
        //    lsChild.load(key);
        //    lsChild.commit();
        //    lsParent.load(key);
        if (iter->second->valid())
        {
            throw std::runtime_error(
                "Valid LedgerEntryReference already exists for this key");
        }

        auto const& entry = iter->second->ignoreInvalid().entry();
        if (!entry)
        {
            throw std::runtime_error(
                "Key exists in memory but LedgerEntry has been erased");
        }

        if (!mChild)
        {
            auto const& previous =
                iter->second->ignoreInvalid().previousEntry();
            ler = makeStateEntry(entry, previous);
        }
        else
        {
            ler = makeStateEntry(entry, entry);
        }
    }
    else if (mParent)
    {
        ler = mParent->loadHelper(key);
    }
    else
    {
        ler = loadFromDatabase(key);
        if (!ler)
        {
            throw std::runtime_error("Key does not exist in database");
        }
    }
    return ler;
}

LedgerState::StateHeader
LedgerState::loadHeader()
{
    assert(mParent || mRoot);
    assert(!mChild);

    mHeader = loadHeaderHelper();
    return mHeader;
}

LedgerState::StateHeader
LedgerState::loadHeaderHelper()
{
    StateHeader lhr;

    if (mHeader)
    {
        if (mHeader->valid())
        {
            throw std::runtime_error("Valid LedgerHeader already exists");
        }

        auto const& header = mHeader->ignoreInvalid().header();
        if (!mChild)
        {
            auto const& previous = mHeader->ignoreInvalid().previousHeader();
            lhr = makeStateHeader(header, previous);
        }
        else
        {
            lhr = makeStateHeader(header, header);
        }
    }
    else if (mParent)
    {
        lhr = mParent->loadHeaderHelper();
    }
    else
    {
        lhr = makeStateHeader(mRoot->getCurrentHeader(),
                              mRoot->getCurrentHeader());
    }
    return lhr;
}

LedgerState::StateEntry
LedgerState::loadFromDatabase(LedgerKey const& key)
{
    assert(mRoot);

    auto cacheKey = binToHex(xdr::xdr_to_opaque(key));
    if (mRoot->getCache().exists(cacheKey))
    {
        auto cached = mRoot->getCache().get(cacheKey);
        if (cached)
        {
            return makeStateEntry(cached, cached);
        }
        else
        {
            return nullptr;
        }
    }

    StateEntry ler;
    switch (key.type())
    {
    case ACCOUNT:
        ler = loadAccountFromDatabase(key);
        break;
    case OFFER:
        ler = loadOfferFromDatabase(key);
        break;
    case TRUSTLINE:
        ler = loadTrustLineFromDatabase(key);
        break;
    case DATA:
        ler = loadDataFromDatabase(key);
        break;
    default:
        abort();
    }

    if (ler)
    {
        mRoot->getCache().put(
            cacheKey, std::make_shared<LedgerEntry const>(*ler->entry()));
    }
    else
    {
        mRoot->getCache().put(cacheKey, nullptr);
    }
    return ler;
}

void
LedgerState::storeInDatabase(StateEntry const& state)
{
    assert(mRoot);
    assert(state->ignoreInvalid().entry());

    auto const& entry = *state->ignoreInvalid().entry();
    auto key = LedgerEntryKey(entry);
    auto cacheKey = binToHex(xdr::xdr_to_opaque(key));
    mRoot->getCache().put(cacheKey, std::make_shared<LedgerEntry const>(entry));

    switch (entry.data.type())
    {
    case ACCOUNT:
        storeAccountInDatabase(state);
        break;
    case OFFER:
        storeOfferInDatabase(state);
        break;
    case TRUSTLINE:
        storeTrustLineInDatabase(state);
        break;
    case DATA:
        storeDataInDatabase(state);
        break;
    default:
        abort();
    }
}

void
LedgerState::deleteFromDatabase(StateEntry const& state)
{
    assert(mRoot);
    assert(!state->ignoreInvalid().entry());

    if (!state->ignoreInvalid().previousEntry())
    {
        return;
    }

    auto const& entry = *state->ignoreInvalid().previousEntry();
    auto key = LedgerEntryKey(entry);
    auto cacheKey = binToHex(xdr::xdr_to_opaque(key));
    mRoot->getCache().put(cacheKey, nullptr);

    switch (entry.data.type())
    {
    case ACCOUNT:
        deleteAccountFromDatabase(state);
        break;
    case OFFER:
        deleteOfferFromDatabase(state);
        break;
    case TRUSTLINE:
        deleteTrustLineFromDatabase(state);
        break;
    case DATA:
        deleteDataFromDatabase(state);
        break;
    default:
        abort();
    }
}

LedgerState::StateEntry
LedgerState::loadBestOffer(Asset const& selling, Asset const& buying)
{
    assert(mParent || mRoot);
    assert(!mChild);
    return getLoadBestOfferContext(selling, buying)->loadBestOffer();
}

std::shared_ptr<LedgerState::LoadBestOfferContext>
LedgerState::getLoadBestOfferContext(Asset const& selling, Asset const& buying)
{
    if (!mLoadBestOfferContext ||
        !(mLoadBestOfferContext->sellingAsset() == selling) ||
        !(mLoadBestOfferContext->buyingAsset() == buying))
    {
        mLoadBestOfferContext = std::make_shared<LoadBestOfferContext>(
                selling, buying, *this);
    }
    return mLoadBestOfferContext;
}

void
LedgerState::invalidateLoadBestOfferContext(Asset const& selling,
                                            Asset const& buying)
{
    if (mLoadBestOfferContext &&
        (mLoadBestOfferContext->sellingAsset() == selling) &&
        (mLoadBestOfferContext->buyingAsset() == buying))
    {
        mLoadBestOfferContext.reset();
    }
}

std::vector<LedgerState::StateEntry>
LedgerState::getOffers(Asset const& selling, Asset const& buying)
{
    std::vector<StateEntry> offers;
    std::set<LedgerKey> seen;
    getOffers(selling, buying, offers, seen);
    return offers;
}

void
LedgerState::getOffers(Asset const& selling, Asset const& buying,
                       std::vector<StateEntry>& offers,
                       std::set<LedgerKey>& seen)
{
    for (auto const& state : mState)
    {
        if (state.first.type() != OFFER)
        {
            continue;
        }
        else if (!seen.insert(state.first).second)
        {
            continue;
        }
        else if (!state.second->ignoreInvalid().entry())
        {
            continue;
        }
        assert(!state.second->valid());

        // Note: Can't compare Assets with !=
        auto const& offer = state.second->ignoreInvalid().entry()->data.offer();
        if (!(selling == offer.selling) || !(buying == offer.buying))
        {
            continue;
        }
        offers.push_back(state.second);
    }

    if (mParent)
    {
        mParent->getOffers(selling, buying, offers, seen);
    }
}

bool
LedgerState::isInMemory(LedgerKey const& key)
{
    if (mState.find(key) != mState.end())
    {
        return true;
    }
    else if (mParent)
    {
        return mParent->isInMemory(key);
    }
    else
    {
        return false;
    }
}

LedgerState::LoadBestOfferContext::LoadBestOfferContext(
    Asset const& selling, Asset const& buying, LedgerState& ledgerState)
    : mSelling(selling)
    , mBuying(buying)
    , mLedgerState(ledgerState)
    , mTop(nullptr)
    , mInMemory([](StateEntry const& lhs,
                   StateEntry const& rhs) { return compareOffers(rhs, lhs); },
                mLedgerState.getOffers(selling, buying))
    , mOffersLoadedFromDatabase(0)
{
}

LedgerState::LoadBestOfferContext::LoadBestOfferContext(
    LoadBestOfferContext&& lboc, LedgerState& ledgerState)
    : mSelling(lboc.mSelling)
    , mBuying(lboc.mBuying)
    , mLedgerState(ledgerState)
    , mTop(lboc.mTop)
    , mInMemory(lboc.mInMemory)
    , mFromDatabase(lboc.mFromDatabase)
    , mOffersLoadedFromDatabase(lboc.mOffersLoadedFromDatabase)
{
}

void
LedgerState::LoadBestOfferContext::loadFromDatabaseIfNecessary()
{
    if (mFromDatabase.empty())
    {
        LedgerState* root = &mLedgerState;
        while (root->mParent != nullptr)
        {
            root = root->mParent;
        }
        mFromDatabase = root->loadBestOffersFromDatabase(
            5, mOffersLoadedFromDatabase, mSelling, mBuying);
        mOffersLoadedFromDatabase += mFromDatabase.size();
    }
}

Asset const&
LedgerState::LoadBestOfferContext::sellingAsset()
{
    return mSelling;
}

Asset const&
LedgerState::LoadBestOfferContext::buyingAsset()
{
    return mBuying;
}

// loadBestOffer has analogous functionality to std::priority_queue<...>::top()
// which returns the data but does not remove it from the data structure. For
// example, the following test case should succeed:
//	Asset a1, a2 = ...;
//	LedgerState ls(...);
//	auto ler1 = ls.loadBestOffer(a1, a2);
//	auto le1 = *ler1->entry();
//	ler1->invalidate();
//	auto ler2 = ls.loadBestOffer(a1, a2);
//	auto le2 = *ler2->entry();
//	REQUIRE(le1 == le2);
// If you desire behavior like top() followed by pop() then code like the
// following is required:
//      Asset a1, a2 = ...;
//      LedgerState ls(...);
//      auto ler1 = ls.loadBestOffer(a1, a2);
//      auto le1 = *ler1->entry();
//      ler1->erase(); // Now we have popped
//      ler1->invalidate();
//      auto ler2 = ls.loadBestOffer(a1, a2);
//      auto le2 = *ler2->entry();
//      REQUIRE(le1 != le2);
// This functionality is natural given that there are cases where you want to
// load the best offer but do not want to erase it.
//
// LoadBestOfferContext maintains the invariant that there is at most one valid
// LedgerEntryReference referring to an offer (with the correct assets), and if
// there is one then it is the one that was just returned by loadBestOffer. This
// means it is incorrect to call loadBestOffer if any LedgerEntryReference
// referring to an offer (with the correct assets) is valid.
//
// Why is this required though? I think it would be possible to implement this
// without this constraint, but it would make it impossible to do the
// performance optimization which is done in LoadBestOfferContext. Specifically,
// if there could be multiple valid LedgerEntryReferences referring to offers
// (with the correct assets) then LoadBestOfferContext::loadBestOffer would have
// to re-process the entire mInMemory on each invocation. It is easy to see that
// this is the case. Any valid LedgerEntryReference can be modified at any time
// (and there is no callback to notify LedgerState that a modification has
// occurred). Suppose that I invoke LoadBestOfferContext::loadBestOffer, then
// modify many offers (with the correct assets), then call
// LoadBestOfferContext::loadBestOffer again. At this point mInMemory could be
// invalid (in the heap sense) at arbitrarily many locations, so it would need
// to be re-formed into a heap. But that would be no better than just iterating
// to find the best offer at every invocation. By making the restriction that if
// a LedgerEntryReference referring to an offer (with the correct assets) is
// only valid if it was just returned by loadBestOffer then we know that
// modifications can only occur at the top of the heap.
//
// The consequence of this is that a sequence of calls to loadBestOffer can be
// efficient if you do not make any intervening calls to LedgerState::load or
// LedgerState::create that constructed a LedgerEntryReference referring to an
// offer (with the correct assets). If such an intervening call is made, then
// the LoadBestOfferContext is destroyed so that it has to be reconstructed at
// the next call to loadBestOffer, thereby maintaining the constraint.
LedgerState::StateEntry
LedgerState::LoadBestOfferContext::loadBestOffer()
{
    if (mTop)
    {
        // This condition is needed to ensure that the following examples
        // work correctly:
        //      Asset a1, a2 = ...;
        //      LedgerState ls(...);
        //      ls.loadBestOffer(a1, a2)->erase();
        //      auto ler = ls.loadBestOffer(a1, a2);
        //      ...
        // and:
        //      Asset a1, a2 = ...;
        //      LedgerState ls(...);
        //      *ls.loadBestOffer(a1, a2)->entry() = ...;
        //      auto ler = ls.loadBestOffer(a1, a2);
        //      ...
        // In the first case, we end up resetting mTop so that the next data is
        // returned. In the second case, mTop still refers to a valid in-memory
        // value so we push it back onto the heap before loading the next data.
        // In the second case, it is possible that mTop is returned again (if it
        // is still the best offer) or that a different offer is returned.
        assert(!mTop->valid());
        if (mTop->ignoreInvalid().entry())
        {
            mInMemory.push(mTop);
        }
        mTop.reset();
    }

    while (!mTop)
    {
        loadFromDatabaseIfNecessary();
        if (mInMemory.empty())
        {
            if (mFromDatabase.empty())
            {
                return nullptr;
            }
            else
            {
                // In this case, there are offers (with the correct assets) in
                // memory. It is possible, then, that the best offer in the
                // database is already loaded in memory. If that is the case, we
                // must skip the result from the database. Then we repeat the
                // entire process (not just this branch) to find the next data.
                mTop = mFromDatabase.front();
                mFromDatabase.erase(mFromDatabase.begin());
                if (mLedgerState.isInMemory(LedgerEntryKey(*mTop->entry())))
                {
                    mTop.reset();
                }
            }
        }
        else if (mFromDatabase.empty() ||
                 compareOffers(mInMemory.top(), mFromDatabase.front()))
        {
            mTop = mInMemory.top();
            mInMemory.pop();
        }
        else
        {
            // In this case, there are offers (with the correct assets) in
            // memory. It is possible, then, that the best offer in the
            // database is already loaded in memory. If that is the case, we
            // must skip the result from the database. Then we repeat the
            // entire process (not just this branch) to find the next data.
            mTop = mFromDatabase.front();
            mFromDatabase.erase(mFromDatabase.begin());
            if (mLedgerState.isInMemory(LedgerEntryKey(*mTop->entry())))
            {
                mTop.reset();
            }
        }
    }

    auto const& entry = mTop->ignoreInvalid().entry();
    auto key = LedgerEntryKey(*entry);
    auto iter = mLedgerState.mState.find(key);
    if (iter != mLedgerState.mState.end())
    {
        auto const& previous = iter->second->ignoreInvalid().previousEntry();
        mTop = mLedgerState.makeStateEntry(entry, previous);
    }
    else
    {
        mTop = mLedgerState.makeStateEntry(entry, entry);
    }
    mLedgerState.mState[key] = mTop;

    return mTop;
}

bool
LedgerState::LoadBestOfferContext::compareOffers(StateEntry const& lhsState,
                                                 StateEntry const& rhsState)
{
    auto const& lhs = lhsState->ignoreInvalid().entry()->data.offer();
    auto const& rhs = rhsState->ignoreInvalid().entry()->data.offer();
    // TODO(jonjove): Should this use the raw price rather than the double price?
    double lhsPrice = double(lhs.price.n) / double(lhs.price.d);
    double rhsPrice = double(rhs.price.n) / double(rhs.price.d);
    if (lhsPrice < rhsPrice)
    {
        return true;
    }
    else if (lhsPrice == rhsPrice)
    {
        return lhs.offerID < rhs.offerID;
    }
    else
    {
        return false;
    }
}

std::vector<InflationVotes>
LedgerState::loadInflationWinners(size_t maxWinners, int64_t minBalance)
{
    LedgerState* root = this;
    while (root->mParent != nullptr)
    {
        root = root->mParent;
    }
    soci::transaction sqlTx(root->mRoot->getDatabase().getSession());

    std::set<LedgerKey> seen;
    for (LedgerState* ls = this; ls; ls = ls->mParent)
    {
        for (auto const& state : ls->mState)
        {
            if (state.first.type() != ACCOUNT)
            {
                continue;
            }
            else if (!seen.insert(state.first).second)
            {
                continue;
            }

            if (state.second->ignoreInvalid().entry())
            {
                root->storeInDatabase(state.second);
            }
            else
            {
                root->deleteFromDatabase(state.second);
            }
        }
    }
    root->mRoot->getCache().clear();

    // Note: When this SQL query returns, the transaction will rollback.
    return root->loadInflationWinnersFromDatabase(maxWinners, minBalance);
}

void
LedgerState::forget(StateEntry se)
{
    assert(mParent || mRoot);
    assert(!mChild);

    assert(se);
    assert(se->valid());

    if (!mHeader)
    {
        loadHeader()->invalidate();
    }
    if (mHeader->ignoreInvalid().header().ledgerVersion >= 8)
    {
        assert(se->entry());
        assert(se->previousEntry());
        assert(*se->entry() == *se->previousEntry());
    }

    auto key = LedgerEntryKey(*se->entry());
    auto iter = mState.find(key);
    assert(iter != mState.end());
    assert(iter->second == se);

    if (key.type() == OFFER)
    {
        auto le = iter->second->entry();
        if (le)
        {
            invalidateLoadBestOfferContext(le->data.offer().selling,
                                           le->data.offer().buying);
        }
    }

    iter->second->invalidate();
    mState.erase(iter);
}

LedgerState::StateEntry
LedgerState::makeStateEntry(std::shared_ptr<LedgerEntry const> const& entry,
                            std::shared_ptr<LedgerEntry const> const& previous)
{
    // Can't use std::make_shared here since LedgerEntryReference has a private
    // constructor (note that std::make_shared does not have access to a private
    // method).
    return StateEntry(new LedgerEntryReference(entry, previous));
}

LedgerState::StateHeader
LedgerState::makeStateHeader(LedgerHeader const& header,
                             LedgerHeader const& previous)
{
    // Can't use std::make_shared here since LedgerHeaderReference has a private
    // constructor (note that std::make_shared does not have access to a private
    // method).
    return StateHeader(new LedgerHeaderReference(header, previous));
}

LedgerKey const&
LedgerState::IteratorValueType::key() const
{
    return mIter->first;
}

std::shared_ptr<LedgerEntry const>
LedgerState::IteratorValueType::entry() const
{
    return mIter->second->ignoreInvalid().entry();
}

std::shared_ptr<LedgerEntry const>
LedgerState::IteratorValueType::previousEntry() const
{
    return mIter->second->ignoreInvalid().previousEntry();
}

LedgerState::Iterator::Iterator(std::map<LedgerKey, StateEntry>::const_iterator const& iter)
{
    mValue.mIter = iter;
}

LedgerState::IteratorValueType const&
LedgerState::Iterator::operator*() const
{
    return mValue;
}

LedgerState::IteratorValueType const*
LedgerState::Iterator::operator->() const
{
    return &mValue;
}

LedgerState::Iterator&
LedgerState::Iterator::operator++()
{
    ++mValue.mIter;
    return *this;
}

bool
LedgerState::Iterator::operator==(Iterator const& other) const
{
    return mValue.mIter == other.mValue.mIter;
}

bool
LedgerState::Iterator::operator!=(Iterator const& other) const
{
    return !(*this == other);
}

LedgerState::Iterator
LedgerState::begin() const
{
    return Iterator(mState.begin());
}

LedgerState::Iterator
LedgerState::end() const
{
    return Iterator(mState.end());
}

void
LedgerState::updateLastModified()
{
    if (!mHeader)
    {
        loadHeader()->invalidate();
    }
    auto ledgerSeq = mHeader->ignoreInvalid().header().ledgerSeq;
    for (auto& state : mState)
    {
        auto entry = state.second->ignoreInvalid().entry();
        if (entry)
        {
            entry->lastModifiedLedgerSeq = ledgerSeq;
        }
    }
}

LedgerKey
LedgerEntryKey(LedgerEntry const& e)
{
    auto& d = e.data;
    LedgerKey k;
    switch (d.type())
    {

    case ACCOUNT:
        k.type(ACCOUNT);
        k.account().accountID = d.account().accountID;
        break;

    case TRUSTLINE:
        k.type(TRUSTLINE);
        k.trustLine().accountID = d.trustLine().accountID;
        k.trustLine().asset = d.trustLine().asset;
        break;

    case OFFER:
        k.type(OFFER);
        k.offer().sellerID = d.offer().sellerID;
        k.offer().offerID = d.offer().offerID;
        break;

    case DATA:
        k.type(DATA);
        k.data().accountID = d.data().accountID;
        k.data().dataName = d.data().dataName;
        break;
    }
    return k;
}
}
