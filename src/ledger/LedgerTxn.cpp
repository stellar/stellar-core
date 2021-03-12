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
#include "ledger/NonSociRelatedException.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <soci.h>

namespace stellar
{

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
populateLoadedEntries(UnorderedSet<LedgerKey> const& keys,
                      std::vector<LedgerEntry> const& entries)
{
    UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>> res;

    for (auto const& le : entries)
    {
        auto key = LedgerEntryKey(le);

        // Abort if two entries for the same key appear.
        assert(res.find(key) == res.end());

        // Only return entries for keys that were actually requested.
        if (keys.find(key) != keys.end())
        {
            res.emplace(key, std::make_shared<LedgerEntry const>(le));
        }
    }

    for (auto const& key : keys)
    {
        if (res.find(key) == res.end())
        {
            res.emplace(key, nullptr);
        }
    }
    return res;
}

bool
operator==(OfferDescriptor const& lhs, OfferDescriptor const& rhs)
{
    return lhs.price == rhs.price && lhs.offerID == rhs.offerID;
}

bool
IsBetterOfferComparator::operator()(OfferDescriptor const& lhs,
                                    OfferDescriptor const& rhs) const
{
    return isBetterOffer(lhs, rhs);
}

bool
operator==(AssetPair const& lhs, AssetPair const& rhs)
{
    return lhs.buying == rhs.buying && lhs.selling == rhs.selling;
}

size_t
AssetPairHash::operator()(AssetPair const& key) const
{
    std::hash<Asset> hashAsset;
    return hashAsset(key.buying) ^ (hashAsset(key.selling) << 1);
}

#ifdef BEST_OFFER_DEBUGGING
void
compareOffers(std::shared_ptr<LedgerEntry const> debugBest,
              std::shared_ptr<LedgerEntry const> best)
{
    if ((bool)debugBest ^ (bool)best)
    {
        if (best)
        {
            printErrorAndAbort("best offer not null when it should be");
        }
        else
        {
            printErrorAndAbort("best offer null when it should not be");
        }
    }
    if (best && *best != *debugBest)
    {
        printErrorAndAbort("best offer mismatch");
    }
}
#endif

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

EntryIterator::EntryIterator(EntryIterator const& other)
    : mImpl(other.mImpl->clone())
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

EntryIterator&
EntryIterator::operator++()
{
    getImpl()->advance();
    return *this;
}

EntryIterator::operator bool() const
{
    return !getImpl()->atEnd();
}

InternalLedgerEntry const&
EntryIterator::entry() const
{
    return getImpl()->entry();
}

bool
EntryIterator::entryExists() const
{
    return getImpl()->entryExists();
}

InternalLedgerKey const&
EntryIterator::key() const
{
    return getImpl()->key();
}

// Implementation of WorstBestOfferIterator -----------------------------------
WorstBestOfferIterator::WorstBestOfferIterator(
    std::unique_ptr<AbstractImpl>&& impl)
    : mImpl(std::move(impl))
{
}

WorstBestOfferIterator::WorstBestOfferIterator(WorstBestOfferIterator&& other)
    : mImpl(std::move(other.mImpl))
{
}

WorstBestOfferIterator::WorstBestOfferIterator(
    WorstBestOfferIterator const& other)
    : mImpl(other.mImpl->clone())
{
}

std::unique_ptr<WorstBestOfferIterator::AbstractImpl> const&
WorstBestOfferIterator::getImpl() const
{
    if (!mImpl)
    {
        throw std::runtime_error("Iterator is empty");
    }
    return mImpl;
}

WorstBestOfferIterator&
WorstBestOfferIterator::operator++()
{
    getImpl()->advance();
    return *this;
}

WorstBestOfferIterator::operator bool() const
{
    return !getImpl()->atEnd();
}

AssetPair const&
WorstBestOfferIterator::assets() const
{
    return getImpl()->assets();
}

std::shared_ptr<OfferDescriptor const> const&
WorstBestOfferIterator::offerDescriptor() const
{
    return getImpl()->offerDescriptor();
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
    , mConsistency(LedgerTxnConsistency::EXACT)
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
        throw std::runtime_error("LedgerTxn was handled");
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

    try
    {
        for (auto const& kv : mActive)
        {
            updateEntryIfRecorded(kv.first, false);
        }
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error during add child to LedgerTxn: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error during add child to LedgerTxn");
    }

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
LedgerTxn::Impl::throwIfNotExactConsistency() const
{
    if (mConsistency != LedgerTxnConsistency::EXACT)
    {
        throw std::runtime_error("LedgerTxn consistency level is not exact");
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
        mParent.commitChild(getEntryIterator(entries), mConsistency);
    });
}

void
LedgerTxn::commitChild(EntryIterator iter, LedgerTxnConsistency cons)
{
    getImpl()->commitChild(std::move(iter), cons);
}

static LedgerTxnConsistency
joinConsistencyLevels(LedgerTxnConsistency c1, LedgerTxnConsistency c2)
{
    switch (c1)
    {
    case LedgerTxnConsistency::EXACT:
        return c2;
    case LedgerTxnConsistency::EXTRA_DELETES:
        return LedgerTxnConsistency::EXTRA_DELETES;
    default:
        abort();
    }
}

void
LedgerTxn::Impl::commitChild(EntryIterator iter, LedgerTxnConsistency cons)
{
    // Assignment of xdrpp objects does not have the strong exception safety
    // guarantee, so use std::unique_ptr<...>::swap to achieve it
    auto childHeader = std::make_unique<LedgerHeader>(mChild->getHeader());

    mConsistency = joinConsistencyLevels(mConsistency, cons);

    try
    {
        for (; (bool)iter; ++iter)
        {
            auto const& key = iter.key();

            if (iter.entryExists())
            {
                updateEntry(
                    key, std::make_shared<InternalLedgerEntry>(iter.entry()));
            }
            else
            {
                updateEntry(key, nullptr);
            }
        }

        // We will show that the following update procedure leaves the self
        // worst best offer map in a correct state.
        //
        // Fix an asset pair P in the child worst best offer map, and let V be
        // the value associated with P. By definition, every offer in
        // LtEq[Self, P, V] has been recorded in the child.
        //
        // Note that LtEq[Self, P, V] contains (among other things):
        //
        // - Every offer in LtEq[Parent, P, V] that was not recorded in self
        //
        // - Every offer in LtEq[Parent, P, V] that was recorded in self and was
        //   not erased, not modified to a different asset pair, and not
        //   modified to be worse than V
        //
        // and does not contain (among other things):
        //
        // - Every offer in LtEq[Parent, P, V] that was recorded in self and
        //   erased, modified to a different asset pair, or modified to be worse
        //   than V
        //
        // The union of these three groups is LtEq[Parent, P, V]. Then
        // we can say that every offer in LtEq[Parent, P, V] is either
        // in LtEq[Self, P, V] or is recorded in self. But because every
        // offer in LtEq[Self, P, V] is recorded in child, after the
        // commit we know that every offer in LtEq[Parent, P, V] must be
        // recorded in self.
        //
        // In the above lemma, we proved that LtEq[Parent, P, V] must be
        // recorded in self after the commit. But it is possible that P was in
        // the self worst best offer map before the commit, with associated
        // value W. In that case, we also know that every offer in
        // NotWorstThan[Parent, P, W] is recorded in self after the commit
        // because they were already recorded in self before the commit. It is
        // clear from the definition of LtEq that
        //
        // - LtEq[Parent, P, V] contains LtEq[Parent, P, W] if W <= V, or
        //
        // - LtEq[Parent, P, W] contains LtEq[Parent, P, V] if V <= W
        //
        // (it is possible that both statements are true if V = W).
        //
        // Then we can conclude that if
        //
        //     Z = (W <= V) ? V : W
        //
        // then every offer in LtEq[Parent, P, Z] is recorded in self after the
        // commit.
        //
        // It follows from these two results that the following update procedure
        // is correct for each asset pair P in the child worst best offer map
        // with associated value V:
        //
        // - If P is not in the self worst best offer map, then insert (P, V)
        //   into the self worst best offer map
        //
        // - If P is in the self worst best offer map with associated value W,
        //   then update (P, W) to (P, V) if V > W and do nothing otherwise
        //
        // Fix an asset pair P that is not in the child worst best offer map. In
        // this case, the child provides no new information. If P is in the self
        // worst best offer map with associated value W, then we know that every
        // offer in LtEq[Self, P, W] was recorded in self prior to the
        // commit so they still will be recorded in self after the commit. If P
        // is also not in the self worst best offer map, then there is no claim
        // about the offers with asset pair P that exist in parent and have been
        // recorded in self both before and after the commit. In either case,
        // there is no need to update the self worst best offer map.
        auto wboIter = mChild->getWorstBestOfferIterator();
        for (; (bool)wboIter; ++wboIter)
        {
            auto fromChild = wboIter.offerDescriptor();
            std::shared_ptr<OfferDescriptor const> descPtr =
                fromChild ? std::make_shared<OfferDescriptor const>(*fromChild)
                          : nullptr;
            updateWorstBestOffer(wboIter.assets(), descPtr);
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
LedgerTxn::create(InternalLedgerEntry const& entry)
{
    return getImpl()->create(*this, entry);
}

LedgerTxnEntry
LedgerTxn::Impl::create(LedgerTxn& self, InternalLedgerEntry const& entry)
{
    throwIfSealed();
    throwIfChild();

    auto key = entry.toKey();
    if (getNewestVersion(key))
    {
        throw std::runtime_error("Key already exists");
    }

    auto current = std::make_shared<InternalLedgerEntry>(entry);
    auto impl = LedgerTxnEntry::makeSharedImpl(self, *current);

    // Set the key to active before constructing the LedgerTxnEntry, as this
    // can throw and the LedgerTxnEntry destructor requires that mActive
    // contains key. LedgerTxnEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerTxnEntry ltxe(impl);

    updateEntry(key, current);
    return ltxe;
}

void
LedgerTxn::createOrUpdateWithoutLoading(InternalLedgerEntry const& entry)
{
    return getImpl()->createOrUpdateWithoutLoading(*this, entry);
}

void
LedgerTxn::Impl::createOrUpdateWithoutLoading(LedgerTxn& self,
                                              InternalLedgerEntry const& entry)
{
    throwIfSealed();
    throwIfChild();

    auto key = entry.toKey();
    auto iter = mActive.find(key);
    if (iter != mActive.end())
    {
        throw std::runtime_error("Key is already active");
    }

    updateEntry(key, std::make_shared<InternalLedgerEntry>(entry));
}

void
LedgerTxn::deactivate(InternalLedgerKey const& key)
{
    getImpl()->deactivate(key);
}

void
LedgerTxn::Impl::deactivate(InternalLedgerKey const& key)
{
    auto iter = mActive.find(key);
    if (iter == mActive.end())
    {
        throw std::runtime_error("Key is not active");
    }

    updateEntryIfRecorded(key, false);

    // C++14 requirements for exception safety of containers guarantee that
    // erase(iter) does not throw
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
LedgerTxn::erase(InternalLedgerKey const& key)
{
    getImpl()->erase(key);
}

void
LedgerTxn::Impl::erase(InternalLedgerKey const& key)
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

    updateEntry(key, nullptr, false);
    // Note: Cannot throw after this point because the entry will not be
    // deactivated in that case

    // C++14 requirements for exception safety of containers guarantee that
    // erase(iter) does not throw
    if (isActive)
    {
        mActive.erase(activeIter);
    }
}

void
LedgerTxn::eraseWithoutLoading(InternalLedgerKey const& key)
{
    getImpl()->eraseWithoutLoading(key);
}

void
LedgerTxn::Impl::eraseWithoutLoading(InternalLedgerKey const& key)
{
    throwIfSealed();
    throwIfChild();

    auto activeIter = mActive.find(key);
    bool isActive = activeIter != mActive.end();

    updateEntry(key, nullptr, false, false);
    // Note: Cannot throw after this point because the entry will not be
    // deactivated in that case

    if (isActive)
    {
        // C++14 requirements for exception safety of containers guarantee that
        // erase(iter) does not throw
        mActive.erase(activeIter);
    }
    mConsistency = LedgerTxnConsistency::EXTRA_DELETES;
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxn::getAllOffers()
{
    return getImpl()->getAllOffers();
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxn::Impl::getAllOffers()
{
    auto offers = mParent.getAllOffers();
    for (auto const& kv : mEntry)
    {
        auto const& key = kv.first;
        auto const& entry = kv.second;
        if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY ||
            key.ledgerKey().type() != OFFER)
        {
            continue;
        }
        if (!entry)
        {
            offers.erase(key.ledgerKey());
            continue;
        }
        // This can throw, but getAllOffers only has the basic exception safety
        // guarantee anyway.
        offers[key.ledgerKey()] = entry->ledgerEntry();
    }
    return offers;
}

#ifdef BEST_OFFER_DEBUGGING
bool
LedgerTxn::bestOfferDebuggingEnabled() const
{
    return getImpl()->bestOfferDebuggingEnabled();
}

bool
LedgerTxn::Impl::bestOfferDebuggingEnabled() const
{
    return mParent.bestOfferDebuggingEnabled();
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::getBestOfferSlow(Asset const& buying, Asset const& selling,
                            OfferDescriptor const* worseThan,
                            std::unordered_set<int64_t>& exclude)
{
    return getImpl()->getBestOfferSlow(buying, selling, worseThan, exclude);
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::Impl::getBestOfferSlow(Asset const& buying, Asset const& selling,
                                  OfferDescriptor const* worseThan,
                                  std::unordered_set<int64_t>& exclude)
{
    std::shared_ptr<InternalLedgerEntry const> selfBest;
    for (auto const& kv : mEntry)
    {
        if (kv.first.type() != InternalLedgerEntryType::LEDGER_ENTRY)
        {
            continue;
        }

        auto const& key = kv.first.ledgerKey();
        if (key.type() != OFFER)
        {
            continue;
        }

        if (!exclude.insert(key.offer().offerID).second)
        {
            continue;
        }

        if (kv.second)
        {
            auto const& le = kv.second->ledgerEntry();
            if (!(le.data.offer().buying == buying &&
                  le.data.offer().selling == selling))
            {
                continue;
            }

            if (worseThan && !isBetterOffer(*worseThan, le))
            {
                continue;
            }

            if (!selfBest || isBetterOffer(le, selfBest->ledgerEntry()))
            {
                selfBest = kv.second;
            }
        }
    }

    auto parentBest =
        mParent.getBestOfferSlow(buying, selling, worseThan, exclude);
    if (selfBest && !parentBest)
    {
        return std::make_shared<LedgerEntry const>(selfBest->ledgerEntry());
    }
    else if (parentBest && !selfBest)
    {
        return std::make_shared<LedgerEntry const>(*parentBest);
    }
    else if (parentBest && selfBest)
    {
        return isBetterOffer(selfBest->ledgerEntry(), *parentBest)
                   ? std::make_shared<LedgerEntry const>(
                         selfBest->ledgerEntry())
                   : std::make_shared<LedgerEntry const>(*parentBest);
    }
    return nullptr;
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::Impl::checkBestOffer(Asset const& buying, Asset const& selling,
                                OfferDescriptor const* worseThan,
                                std::shared_ptr<LedgerEntry const> best)
{
    if (!bestOfferDebuggingEnabled())
    {
        return best;
    }

    std::unordered_set<int64_t> exclude;
    auto debugBest = getBestOfferSlow(buying, selling, worseThan, exclude);
    compareOffers(debugBest, best);
    return best;
}
#endif

std::shared_ptr<LedgerEntry const>
LedgerTxn::getBestOffer(Asset const& buying, Asset const& selling)
{
#ifdef BEST_OFFER_DEBUGGING
    return getImpl()->checkBestOffer(buying, selling, nullptr,
                                     getImpl()->getBestOffer(buying, selling));
#else
    return getImpl()->getBestOffer(buying, selling);
#endif
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::Impl::getBestOffer(Asset const& buying, Asset const& selling)
{
    if (!mActive.empty())
    {
        throw std::runtime_error("active entries when getting best offer");
    }

    AssetPair const assets{buying, selling};

    std::shared_ptr<LedgerEntry const> selfBest;
    auto mobIter = mMultiOrderBook.find(assets);
    if (mobIter != mMultiOrderBook.end())
    {
        auto const& offers = mobIter->second;
        if (!offers.empty())
        {
            auto entryIter = mEntry.find(offers.begin()->second);
            if (entryIter == mEntry.end() || !entryIter->second)
            {
                throw std::runtime_error("invalid order book state");
            }
            selfBest = std::make_shared<LedgerEntry const>(
                entryIter->second->ledgerEntry());
        }
    }

    std::shared_ptr<LedgerEntry const> parentBest;
    auto wboIter = mWorstBestOffer.find(assets);
    if (wboIter != mWorstBestOffer.end())
    {
        // Let P be the asset pair (buying, selling). Because wboIter->second is
        // the worst best offer for P, every offer in
        // LtEq[Parent, P, wboIter->second] is recorded in self. There
        // is no reason to consider any offer in parent that has been recorded
        // in self, because they will be excluded by the loop at the end of this
        // function (see the comment before that loop for more details).
        // Therefore, we get the best offer that existed in the parent and is
        // worse than wboIter->second. But if wboIter->second is nullptr, then
        // there is no such offer by definition so we can avoid the recursive
        // call and simply set parentBest = nullptr.
        if (wboIter->second)
        {
            parentBest =
                mParent.getBestOffer(buying, selling, *wboIter->second);
        }
        // If parentBest is nullptr, then either wboIter->second is nullptr or
        // the parent contains no offers that are worse than wboIter->second. In
        // the first case, we have already returned nullptr for the specified
        // asset pair so we know that that parent does not have any offers that
        // have not been recorded in self. In either case, we always return
        // selfBest (which may also be nullptr).
    }
    else
    {
        parentBest = mParent.getBestOffer(buying, selling);
        // If parentBest is nullptr, then the parent contains no offers with the
        // specified asset pair. Therefore we always return selfBest (which may
        // also be nullptr).
    }

    // If parentBest is not nullptr, then the parent contains at least one offer
    // with the specified asset pair. If selfBest is not nullptr and parentBest
    // is not better than selfBest, then we must return selfBest because
    // parentBest cannot be the best offer. Otherwise, either selfBest is
    // nullptr or parentBest is better than selfBest. In the first case, self
    // has no recorded offer with the specified asset pair. In the second case,
    // self has a recorded offer with the specified asset pair but it is worse
    // than parentBest. Either way, we should return parentBest if it is not
    // recorded in self. If it is recorded in self, then one of the following is
    // true:
    //
    // - it has been erased, in which case it is not the best offer
    //
    // - it has been modified to a different asset pair or a worse price, in
    //   which case it is not the best offer
    //
    // - it has been modified to a price no worse than its price in the parent,
    //   but this possibility is excluded by the fact that either selfBest is
    //   nullptr or parentBest is better than selfBest
    //
    // If parentBest is recorded in self, then we must ask parent for the next
    // best offer that is worse than parentBest and repeat.
    while (parentBest && (!selfBest || isBetterOffer(*parentBest, *selfBest)))
    {
        if (mEntry.find(LedgerEntryKey(*parentBest)) == mEntry.end())
        {
            return parentBest;
        }
        auto const& oe = parentBest->data.offer();
        parentBest =
            mParent.getBestOffer(buying, selling, {oe.price, oe.offerID});
    }
    return selfBest;
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::getBestOffer(Asset const& buying, Asset const& selling,
                        OfferDescriptor const& worseThan)
{
#ifdef BEST_OFFER_DEBUGGING
    return getImpl()->checkBestOffer(
        buying, selling, &worseThan,
        getImpl()->getBestOffer(buying, selling, worseThan));
#else
    return getImpl()->getBestOffer(buying, selling, worseThan);
#endif
}

std::shared_ptr<LedgerEntry const>
LedgerTxn::Impl::getBestOffer(Asset const& buying, Asset const& selling,
                              OfferDescriptor const& worseThan)
{
    if (!mActive.empty())
    {
        throw std::runtime_error("active entries when getting best offer");
    }

    AssetPair const assets{buying, selling};

    std::shared_ptr<LedgerEntry const> selfBest;
    auto mobIter = mMultiOrderBook.find(assets);
    if (mobIter != mMultiOrderBook.end())
    {
        auto const& offers = mobIter->second;
        auto iter = offers.upper_bound(worseThan);
        if (iter != offers.end())
        {
            auto entryIter = mEntry.find(iter->second);
            if (entryIter == mEntry.end() || !entryIter->second)
            {
                throw std::runtime_error("invalid order book state");
            }
            selfBest = std::make_shared<LedgerEntry const>(
                entryIter->second->ledgerEntry());
        }
    }

    auto parentBest = mParent.getBestOffer(buying, selling, worseThan);

    // If parentBest is nullptr, then the parent contains no offers that are
    // worse than worseThan. Therefore we always return selfBest (which may also
    // be nullptr).
    //
    // If parentBest is not nullptr, then the parent contains at least one offer
    // with the specified asset pair that is worse than worseThan. If selfBest
    // is not nullptr and parentBest is not better than selfBest, then we must
    // return selfBest because parentBest cannot be the best offer. Otherwise,
    // either selfBest is nullptr or parentBest is better than selfBest. In the
    // first case, self has no recorded offer with the specified asset pair that
    // is worse than worseThan. In the second case, self has a recorded offer
    // with the specified asset pair that is worse than worseThan but it is also
    // worse than parentBest. Either way, we should return parentBest if it is
    // not recorded in self. If it is recorded in self, then one of the
    // following is true:
    //
    // - it has been erased, in which case it is not the best offer
    //
    // - it has been modified to a different asset pair or a worse price, in
    //   which case it is not the best offer
    //
    // - it has been modified to a price no worse than its price in the parent,
    //   but this possibility is excluded by the fact that either selfBest is
    //   nullptr or parentBest is better than selfBest
    //
    // If parentBest is recorded in self, then we must ask parent for the next
    // best offer that is worse than parentBest and repeat.
    while (parentBest && (!selfBest || isBetterOffer(*parentBest, *selfBest)))
    {
        if (mEntry.find(LedgerEntryKey(*parentBest)) == mEntry.end())
        {
            return parentBest;
        }
        auto const& oe = parentBest->data.offer();
        parentBest =
            mParent.getBestOffer(buying, selling, {oe.price, oe.offerID});
    }
    return selfBest;
}

LedgerEntryChanges
LedgerTxn::getChanges()
{
    return getImpl()->getChanges();
}

LedgerEntryChanges
LedgerTxn::Impl::getChanges()
{
    throwIfNotExactConsistency();
    LedgerEntryChanges changes;
    changes.reserve(mEntry.size() * 2);
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& key = kv.first;
            auto const& entry = kv.second;

            if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY)
            {
                continue;
            }

            auto previous = mParent.getNewestVersion(key);
            if (previous)
            {
                changes.emplace_back(LEDGER_ENTRY_STATE);
                changes.back().state() = previous->ledgerEntry();

                if (entry)
                {
                    changes.emplace_back(LEDGER_ENTRY_UPDATED);
                    changes.back().updated() = entry->ledgerEntry();
                }
                else
                {
                    changes.emplace_back(LEDGER_ENTRY_REMOVED);
                    changes.back().removed() = key.ledgerKey();
                }
            }
            else
            {
                // If !entry and !previous.entry then the entry was created and
                // erased in this LedgerTxn, in which case it should not still
                // be in this LedgerTxn
                assert(entry);
                changes.emplace_back(LEDGER_ENTRY_CREATED);
                changes.back().created() = entry->ledgerEntry();
            }
        }
    });
    return changes;
}

LedgerTxnDelta
LedgerTxn::getDelta()
{
    return getImpl()->getDelta();
}

LedgerTxnDelta
LedgerTxn::Impl::getDelta()
{
    throwIfNotExactConsistency();
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
        if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY ||
            key.ledgerKey().type() != ACCOUNT)
        {
            continue;
        }

        if (entry)
        {
            auto const& acc = entry->ledgerEntry().data.account();
            if (acc.inflationDest && acc.balance >= MIN_VOTES_TO_INCLUDE)
            {
                deltaVotes[*acc.inflationDest] += acc.balance;
            }
        }

        auto previous = mParent.getNewestVersion(key);
        if (previous)
        {
            auto const& acc = previous->ledgerEntry().data.account();
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

void
LedgerTxn::getAllEntries(std::vector<LedgerEntry>& initEntries,
                         std::vector<LedgerEntry>& liveEntries,
                         std::vector<LedgerKey>& deadEntries)
{
    getImpl()->getAllEntries(initEntries, liveEntries, deadEntries);
}

void
LedgerTxn::Impl::getAllEntries(std::vector<LedgerEntry>& initEntries,
                               std::vector<LedgerEntry>& liveEntries,
                               std::vector<LedgerKey>& deadEntries)
{
    std::vector<LedgerEntry> resInit, resLive;
    std::vector<LedgerKey> resDead;
    resInit.reserve(mEntry.size());
    resLive.reserve(mEntry.size());
    resDead.reserve(mEntry.size());
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        for (auto const& kv : entries)
        {
            auto const& key = kv.first;
            auto const& entry = kv.second;

            if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY)
            {
                continue;
            }

            if (entry)
            {
                auto previous = mParent.getNewestVersion(key);
                if (previous)
                {
                    resLive.emplace_back(entry->ledgerEntry());
                }
                else
                {
                    resInit.emplace_back(entry->ledgerEntry());
                }
            }
            else
            {
                resDead.emplace_back(key.ledgerKey());
            }
        }
    });
    initEntries.swap(resInit);
    liveEntries.swap(resLive);
    deadEntries.swap(resDead);
}

std::shared_ptr<InternalLedgerEntry const>
LedgerTxn::getNewestVersion(InternalLedgerKey const& key) const
{
    return getImpl()->getNewestVersion(key);
}

std::shared_ptr<InternalLedgerEntry const>
LedgerTxn::Impl::getNewestVersion(InternalLedgerKey const& key) const
{
    auto iter = mEntry.find(key);
    if (iter != mEntry.end())
    {
        return iter->second;
    }
    return mParent.getNewestVersion(key);
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxn::getOffersByAccountAndAsset(AccountID const& account,
                                      Asset const& asset)
{
    return getImpl()->getOffersByAccountAndAsset(account, asset);
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxn::Impl::getOffersByAccountAndAsset(AccountID const& account,
                                            Asset const& asset)
{
    auto offers = mParent.getOffersByAccountAndAsset(account, asset);
    for (auto const& kv : mEntry)
    {
        auto const& key = kv.first;
        auto const& entry = kv.second;
        if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY ||
            key.ledgerKey().type() != OFFER)
        {
            continue;
        }
        if (!entry)
        {
            offers.erase(key.ledgerKey());
            continue;
        }

        auto const& oe = entry->ledgerEntry().data.offer();
        if (oe.sellerID == account &&
            (oe.selling == asset || oe.buying == asset))
        {
            offers[key.ledgerKey()] = entry->ledgerEntry();
        }
        else
        {
            offers.erase(key.ledgerKey());
        }
    }
    return offers;
}

LedgerTxnEntry
LedgerTxn::load(InternalLedgerKey const& key)
{
    return getImpl()->load(*this, key);
}

LedgerTxnEntry
LedgerTxn::Impl::load(LedgerTxn& self, InternalLedgerKey const& key)
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

    auto current = std::make_shared<InternalLedgerEntry>(*newest);
    auto impl = LedgerTxnEntry::makeSharedImpl(self, *current);

    // Set the key to active before constructing the LedgerTxnEntry, as this
    // can throw and the LedgerTxnEntry destructor requires that mActive
    // contains key. LedgerTxnEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerTxnEntry ltxe(impl);

    // If this throws, the order book will not be modified because of the strong
    // exception safety guarantee. Furthermore, ltxe will be destructed leading
    // to key being deactivated. This will leave LedgerTxn unmodified.
    updateEntry(key, current);
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
    auto previousMultiOrderBook = mMultiOrderBook;
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
        mMultiOrderBook.swap(previousMultiOrderBook);
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

    auto le = getBestOffer(buying, selling);
    auto res = le ? load(self, LedgerEntryKey(*le)) : LedgerTxnEntry();

    try
    {
        std::shared_ptr<OfferDescriptor const> descPtr;
        if (le)
        {
            auto const& oe = le->data.offer();
            descPtr = std::make_shared<OfferDescriptor const>(
                OfferDescriptor{oe.price, oe.offerID});
        }

        // We will show that the following update procedure leaves the worst
        // best offer map in a correct state.
        //
        // Let P be the asset pair (buying, selling). Every offer in
        // LtEq[Parent, P, le] except le must have been recorded in self
        // and erased, modified to a different asset pair, or modified to be
        // worse than le. For if this were not the case then there exists an
        // offer A better than le that exists in self, and this contradicts the
        // fact that le is the best offer with asset pair P that exists in self.
        // But now le has also been recorded in self as well, so every offer in
        // LtEq[Parent, P, le] is recorded in self after loadBestOffer.
        //
        // But it is possible that P was in the self worst best offer map before
        // loadBestOffer, with associated value W. In that case, we also know
        // that every offer in LtEq[Parent, P, W] is recorded in self.
        // It is clear from the definition of LtEq that
        //
        // - LtEq[Parent, P, le] contains LtEq[Parent, P, W] if W <= le,
        //
        // - LtEq[Parent, P, W] contains LtEq[Parent, P, le] if le <= W
        //
        // (it is possible that both statements are true if le = W).
        //
        // Then we can conclude that if
        //
        //     Z = (W <= le) ? le : W
        //
        // then every offer in LtEq[Parent, P, Z] is recorded in self
        // after the commit.
        //
        //   Note that the situation described in this lemma can occur, for
        //   example, if the following sequence occurs:
        //
        //   1. A = loadBestOffer(buying, selling)
        //   2. Create an offer better than A with asset pair (buying, selling)
        //   3. B = loadBestOffer(buying, selling)
        //
        // It follows from these two results that the following update procedure
        // is correct:
        //
        // - If P is not in the self worst best offer map, then insert (P, le)
        //   into the self worst best offer map
        //
        // - If P is in the self worst best offer map with associated value W,
        //   then update (P, W) to (P, le) if le > W and do nothing otherwise

        // updateWorstBestOffer has the strong exception safety guarantee
        updateWorstBestOffer({buying, selling}, descPtr);
    }
    catch (...)
    {
        // We don't need to update mWorstBestOffer, it's just an optimization
    }
    return res;
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
    auto previousMultiOrderBook = mMultiOrderBook;
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
        mMultiOrderBook.swap(previousMultiOrderBook);
        throw;
    }
}

ConstLedgerTxnEntry
LedgerTxn::loadWithoutRecord(InternalLedgerKey const& key)
{
    return getImpl()->loadWithoutRecord(*this, key);
}

ConstLedgerTxnEntry
LedgerTxn::Impl::loadWithoutRecord(LedgerTxn& self,
                                   InternalLedgerKey const& key)
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
    ConstLedgerTxnEntry ltxe(impl);

    // If this throws, the order book will not be modified because of the strong
    // exception safety guarantee. Furthermore, ltxe will be destructed leading
    // to key being deactivated. This will leave LedgerTxn unmodified.
    //
    // If key has not been recorded, this function does nothing. If key has been
    // recorded, this removes the corresponding entry from mMultiOrderBook.
    updateEntryIfRecorded(key, true);
    return ltxe;
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

    mEntry.clear();
    mMultiOrderBook.clear();
    mActive.clear();
    mActiveHeader.reset();
    mIsSealed = true;

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

uint64_t
LedgerTxn::countObjects(LedgerEntryType let) const
{
    throw std::runtime_error("called countObjects on non-root LedgerTxn");
}

uint64_t
LedgerTxn::countObjects(LedgerEntryType let, LedgerRange const& ledgers) const
{
    throw std::runtime_error("called countObjects on non-root LedgerTxn");
}

void
LedgerTxn::deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const
{
    throw std::runtime_error(
        "called deleteObjectsModifiedOnOrAfterLedger on non-root LedgerTxn");
}

void
LedgerTxn::dropAccounts()
{
    throw std::runtime_error("called dropAccounts on non-root LedgerTxn");
}

void
LedgerTxn::dropData()
{
    throw std::runtime_error("called dropData on non-root LedgerTxn");
}

void
LedgerTxn::dropOffers()
{
    throw std::runtime_error("called dropOffers on non-root LedgerTxn");
}

void
LedgerTxn::dropTrustLines()
{
    throw std::runtime_error("called dropTrustLines on non-root LedgerTxn");
}

void
LedgerTxn::dropClaimableBalances()
{
    throw std::runtime_error(
        "called dropClaimableBalances on non-root LedgerTxn");
}

double
LedgerTxn::getPrefetchHitRate() const
{
    return getImpl()->getPrefetchHitRate();
}

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
void
LedgerTxn::resetForFuzzer()
{
    abort();
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

double
LedgerTxn::Impl::getPrefetchHitRate() const
{
    return mParent.getPrefetchHitRate();
}

uint32_t
LedgerTxn::prefetch(UnorderedSet<LedgerKey> const& keys)
{
    return getImpl()->prefetch(keys);
}

uint32_t
LedgerTxn::Impl::prefetch(UnorderedSet<LedgerKey> const& keys)
{
    return mParent.prefetch(keys);
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
        std::shared_ptr<InternalLedgerEntry> entry;
        if (kv.second)
        {
            entry = std::make_shared<InternalLedgerEntry>(*kv.second);
            if (mShouldUpdateLastModified &&
                entry->type() == InternalLedgerEntryType::LEDGER_ENTRY)
            {
                entry->ledgerEntry().lastModifiedLedgerSeq = mHeader->ledgerSeq;
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

        // std::multiset<...>::clear does not throw
        // std::set<...>::clear does not throw
        // std::shared_ptr<...>::reset does not throw
        mMultiOrderBook.clear();
        mActive.clear();
        mActiveHeader.reset();
        mIsSealed = true;
    }
    else // Note: can't have child if sealed
    {
        f(mEntry);
    }
}

std::pair<LedgerTxn::Impl::MultiOrderBook::iterator,
          LedgerTxn::Impl::OrderBook::iterator>
LedgerTxn::Impl::findInOrderBook(LedgerEntry const& le)
{
    auto const& oe = le.data.offer();
    auto mobIter = mMultiOrderBook.find({oe.buying, oe.selling});
    OrderBook::iterator obIter;
    if (mobIter != mMultiOrderBook.end())
    {
        obIter = mobIter->second.find({oe.price, oe.offerID});
    }
    return {mobIter, obIter};
}

void
LedgerTxn::Impl::updateEntryIfRecorded(InternalLedgerKey const& key,
                                       bool effectiveActive)
{
    auto entryIter = mEntry.find(key);
    // If loadWithoutRecord was used, then key may not be in mEntry. But if key
    // is not in mEntry, then there is no reason to have an entry in the order
    // book. Therefore we only updateEntry if key is in mEntry.
    if (entryIter != mEntry.end())
    {
        updateEntry(key, entryIter->second, effectiveActive);
    }
}

void
LedgerTxn::Impl::updateEntry(InternalLedgerKey const& key,
                             std::shared_ptr<InternalLedgerEntry> lePtr)
{
    bool effectiveActive = mActive.find(key) != mActive.end();
    updateEntry(key, lePtr, effectiveActive);
}

void
LedgerTxn::Impl::updateEntry(InternalLedgerKey const& key,
                             std::shared_ptr<InternalLedgerEntry> lePtr,
                             bool effectiveActive)
{
    bool eraseIfNull = !lePtr && !mParent.getNewestVersion(key);
    updateEntry(key, lePtr, effectiveActive, eraseIfNull);
}

void
LedgerTxn::Impl::updateEntry(InternalLedgerKey const& key,
                             std::shared_ptr<InternalLedgerEntry> lePtr,
                             bool effectiveActive, bool eraseIfNull)
{
    // recordEntry has the strong exception safety guarantee because
    // - UnorderedMap<...>::erase has the strong exception safety
    //   guarantee
    // - UnorderedMap<...>::operator[] has the strong exception safety
    //   guarantee
    // - std::shared_ptr<...>::operator= does not throw
    auto recordEntry = [&]() {
        if (eraseIfNull)
        {
            mEntry.erase(key);
        }
        else
        {
            mEntry[key] = lePtr;
        }
    };

    // If the key does not correspond to an offer, we do not need to manage the
    // order book. Record the update in mEntry and return.
    if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY ||
        key.ledgerKey().type() != OFFER)
    {
        recordEntry();
        return;
    }

    OrderBook* obOld = nullptr;
    OrderBook::iterator obIterOld;
    auto iter = mEntry.find(key);
    if (iter != mEntry.end() && iter->second)
    {
        MultiOrderBook::iterator mobIterOld;
        std::tie(mobIterOld, obIterOld) =
            findInOrderBook(iter->second->ledgerEntry());
        if (mobIterOld != mMultiOrderBook.end())
        {
            obOld = &mobIterOld->second;
        }
    }

    // We only insert the new offer into the order book if it exists and is not
    // active. Otherwise, we just record the update in mEntry and return.
    if (lePtr && !effectiveActive)
    {
        auto const& oe = lePtr->ledgerEntry().data.offer();
        AssetPair assetPair{oe.buying, oe.selling};

        auto mobIterNew = mMultiOrderBook.find(assetPair);
        if (mobIterNew != mMultiOrderBook.end())
        {
            auto& obNew = mobIterNew->second;
            // obNew is a reference to an OrderBook, which is a typedef for
            // std::multimap<...>. std::multimap<...> does not invalidate any
            // iterators on insertion, so obIterOld is still valid after this
            // insertion. From the standard:
            //
            //    The insert and emplace members shall not affect the validity
            //    of iterators and references to the container.
            auto res = obNew.insert({{oe.price, oe.offerID}, key.ledgerKey()});
            try
            {
                recordEntry();
            }
            catch (...)
            {
                obNew.erase(res);
                throw;
            }
        }
        else
        {
            // mMultiOrderBook is a MultiOrderBook, which is a typedef for
            // UnorderedMap<...>. UnorderedMap<...> may invalidate
            // all iterators on insertion if a rehash is required, but pointers
            // are guaranteed to remain valid so obOld is still valid after
            // this insertion. From the standard:
            //
            //    The insert and emplace members shall not affect the validity
            //    of references to container elements, but may invalidate all
            //    iterators to the container.
            auto res = mMultiOrderBook.emplace(
                assetPair,
                OrderBook{{{oe.price, oe.offerID}, key.ledgerKey()}});
            try
            {
                recordEntry();
            }
            catch (...)
            {
                mMultiOrderBook.erase(res.first);
                throw;
            }
        }
    }
    else
    {
        recordEntry();
    }

    // This never throws
    if (obOld && obIterOld != obOld->end())
    {
        obOld->erase(obIterOld);
    }
}

static bool
isWorseThan(std::shared_ptr<OfferDescriptor const> const& lhs,
            std::shared_ptr<OfferDescriptor const> const& rhs)
{
    return rhs && (!lhs || isBetterOffer(*rhs, *lhs));
}

void
LedgerTxn::Impl::updateWorstBestOffer(
    AssetPair const& assets, std::shared_ptr<OfferDescriptor const> offerDesc)
{
    // Update mWorstBestOffer if
    // - assets is currently not in mWorstBestOffer
    // - offerDesc is worse than mWorstBestOffer[assets]
    auto iter = mWorstBestOffer.find(assets);
    if (iter == mWorstBestOffer.end() || isWorseThan(offerDesc, iter->second))
    {
        // UnorderedMap<...>::operator[] has the strong exception safety
        // guarantee
        // std::shared_ptr<...>::operator= does not throw
        mWorstBestOffer[assets] = offerDesc;
    }
}

WorstBestOfferIterator
LedgerTxn::getWorstBestOfferIterator()
{
    return getImpl()->getWorstBestOfferIterator();
}

WorstBestOfferIterator
LedgerTxn::Impl::getWorstBestOfferIterator()
{
    auto iterImpl = std::make_unique<WorstBestOfferIteratorImpl>(
        mWorstBestOffer.cbegin(), mWorstBestOffer.cend());
    return WorstBestOfferIterator(std::move(iterImpl));
}

bool
LedgerTxn::hasSponsorshipEntry() const
{
    return getImpl()->hasSponsorshipEntry();
}

bool
LedgerTxn::Impl::hasSponsorshipEntry() const
{
    throwIfNotExactConsistency();
    throwIfChild();

    for (auto const& kv : mEntry)
    {
        auto glk = kv.first;
        switch (glk.type())
        {
        case InternalLedgerEntryType::SPONSORSHIP:
        case InternalLedgerEntryType::SPONSORSHIP_COUNTER:
            return true;
        default:
            break;
        }
    }

    return false;
}

#ifdef BUILD_TESTS
UnorderedMap<AssetPair,
             std::multimap<OfferDescriptor, LedgerKey, IsBetterOfferComparator>,
             AssetPairHash> const&
LedgerTxn::getOrderBook()
{
    return getImpl()->getOrderBook();
}

LedgerTxn::Impl::MultiOrderBook const&
LedgerTxn::Impl::getOrderBook()
{
    return mMultiOrderBook;
}
#endif

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

InternalLedgerEntry const&
LedgerTxn::Impl::EntryIteratorImpl::entry() const
{
    return *(mIter->second);
}

bool
LedgerTxn::Impl::EntryIteratorImpl::entryExists() const
{
    return (bool)(mIter->second);
}

InternalLedgerKey const&
LedgerTxn::Impl::EntryIteratorImpl::key() const
{
    return mIter->first;
}

std::unique_ptr<EntryIterator::AbstractImpl>
LedgerTxn::Impl::EntryIteratorImpl::clone() const
{
    return std::make_unique<EntryIteratorImpl>(mIter, mEnd);
}

// Implementation of LedgerTxn::Impl::WorstBestOfferIteratorImpl --------------
LedgerTxn::Impl::WorstBestOfferIteratorImpl::WorstBestOfferIteratorImpl(
    IteratorType const& begin, IteratorType const& end)
    : mIter(begin), mEnd(end)
{
}

void
LedgerTxn::Impl::WorstBestOfferIteratorImpl::advance()
{
    ++mIter;
}

AssetPair const&
LedgerTxn::Impl::WorstBestOfferIteratorImpl::assets() const
{
    return mIter->first;
}

bool
LedgerTxn::Impl::WorstBestOfferIteratorImpl::atEnd() const
{
    return mIter == mEnd;
}

std::shared_ptr<OfferDescriptor const> const&
LedgerTxn::Impl::WorstBestOfferIteratorImpl::offerDescriptor() const
{
    return mIter->second;
}

std::unique_ptr<WorstBestOfferIterator::AbstractImpl>
LedgerTxn::Impl::WorstBestOfferIteratorImpl::clone() const
{
    return std::make_unique<WorstBestOfferIteratorImpl>(mIter, mEnd);
}

// Implementation of LedgerTxnRoot ------------------------------------------
size_t const LedgerTxnRoot::Impl::MIN_BEST_OFFERS_BATCH_SIZE = 5;

LedgerTxnRoot::LedgerTxnRoot(Database& db, size_t entryCacheSize,
                             size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                             ,
                             bool bestOfferDebuggingEnabled
#endif
                             )
    : mImpl(std::make_unique<Impl>(db, entryCacheSize, prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                                   ,
                                   bestOfferDebuggingEnabled
#endif
                                   ))
{
}

LedgerTxnRoot::Impl::Impl(Database& db, size_t entryCacheSize,
                          size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                          ,
                          bool bestOfferDebuggingEnabled
#endif
                          )
    : mMaxBestOffersBatchSize(
          std::min(std::max(prefetchBatchSize, MIN_BEST_OFFERS_BATCH_SIZE),
                   MAX_OFFERS_TO_CROSS))
    , mDatabase(db)
    , mHeader(std::make_unique<LedgerHeader>())
    , mEntryCache(entryCacheSize)
    , mBulkLoadBatchSize(prefetchBatchSize)
    , mChild(nullptr)
#ifdef BEST_OFFER_DEBUGGING
    , mBestOfferDebuggingEnabled(bestOfferDebuggingEnabled)
#endif
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

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
void
LedgerTxnRoot::Impl::resetForFuzzer()
{
    mBestOffers.clear();
    mEntryCache.clear();
}

void
LedgerTxnRoot::resetForFuzzer()
{
    mImpl->resetForFuzzer();
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

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
LedgerTxnRoot::commitChild(EntryIterator iter, LedgerTxnConsistency cons)
{
    mImpl->commitChild(std::move(iter), cons);
}

static void
accum(EntryIterator const& iter, std::vector<EntryIterator>& upsertBuffer,
      std::vector<EntryIterator>& deleteBuffer)
{
    if (iter.entryExists())
        upsertBuffer.emplace_back(iter);
    else
        deleteBuffer.emplace_back(iter);
}

void
BulkLedgerEntryChangeAccumulator::accumulate(EntryIterator const& iter)
{
    // Right now, only LEDGER_ENTRY are recorded in the SQL database
    if (iter.key().type() != InternalLedgerEntryType::LEDGER_ENTRY)
    {
        return;
    }

    switch (iter.key().ledgerKey().type())
    {
    case ACCOUNT:
        accum(iter, mAccountsToUpsert, mAccountsToDelete);
        break;
    case TRUSTLINE:
        accum(iter, mTrustLinesToUpsert, mTrustLinesToDelete);
        break;
    case OFFER:
        accum(iter, mOffersToUpsert, mOffersToDelete);
        break;
    case DATA:
        accum(iter, mAccountDataToUpsert, mAccountDataToDelete);
        break;
    case CLAIMABLE_BALANCE:
        accum(iter, mClaimableBalanceToUpsert, mClaimableBalanceToDelete);
        break;
    default:
        abort();
    }
}

void
LedgerTxnRoot::Impl::bulkApply(BulkLedgerEntryChangeAccumulator& bleca,
                               size_t bufferThreshold,
                               LedgerTxnConsistency cons)
{
    auto& upsertAccounts = bleca.getAccountsToUpsert();
    if (upsertAccounts.size() > bufferThreshold)
    {
        bulkUpsertAccounts(upsertAccounts);
        upsertAccounts.clear();
    }
    auto& deleteAccounts = bleca.getAccountsToDelete();
    if (deleteAccounts.size() > bufferThreshold)
    {
        bulkDeleteAccounts(deleteAccounts, cons);
        deleteAccounts.clear();
    }
    auto& upsertTrustLines = bleca.getTrustLinesToUpsert();
    if (upsertTrustLines.size() > bufferThreshold)
    {
        bulkUpsertTrustLines(upsertTrustLines);
        upsertTrustLines.clear();
    }
    auto& deleteTrustLines = bleca.getTrustLinesToDelete();
    if (deleteTrustLines.size() > bufferThreshold)
    {
        bulkDeleteTrustLines(deleteTrustLines, cons);
        deleteTrustLines.clear();
    }
    auto& upsertOffers = bleca.getOffersToUpsert();
    if (upsertOffers.size() > bufferThreshold)
    {
        bulkUpsertOffers(upsertOffers);
        upsertOffers.clear();
    }
    auto& deleteOffers = bleca.getOffersToDelete();
    if (deleteOffers.size() > bufferThreshold)
    {
        bulkDeleteOffers(deleteOffers, cons);
        deleteOffers.clear();
    }
    auto& upsertAccountData = bleca.getAccountDataToUpsert();
    if (upsertAccountData.size() > bufferThreshold)
    {
        bulkUpsertAccountData(upsertAccountData);
        upsertAccountData.clear();
    }
    auto& deleteAccountData = bleca.getAccountDataToDelete();
    if (deleteAccountData.size() > bufferThreshold)
    {
        bulkDeleteAccountData(deleteAccountData, cons);
        deleteAccountData.clear();
    }
    auto& upsertClaimableBalance = bleca.getClaimableBalanceToUpsert();
    if (upsertClaimableBalance.size() > bufferThreshold)
    {
        bulkUpsertClaimableBalance(upsertClaimableBalance);
        upsertClaimableBalance.clear();
    }
    auto& deleteClaimableBalance = bleca.getClaimableBalanceToDelete();
    if (deleteClaimableBalance.size() > bufferThreshold)
    {
        bulkDeleteClaimableBalance(deleteClaimableBalance, cons);
        deleteClaimableBalance.clear();
    }
}

void
LedgerTxnRoot::Impl::commitChild(EntryIterator iter, LedgerTxnConsistency cons)
{
    ZoneScoped;
    // Assignment of xdrpp objects does not have the strong exception safety
    // guarantee, so use std::unique_ptr<...>::swap to achieve it
    auto childHeader = std::make_unique<LedgerHeader>(mChild->getHeader());

    auto bleca = BulkLedgerEntryChangeAccumulator();
    int64_t counter{0};
    try
    {
        while ((bool)iter)
        {
            bleca.accumulate(iter);
            ++iter;
            ++counter;
            size_t bufferThreshold =
                (bool)iter ? LEDGER_ENTRY_BATCH_COMMIT_SIZE : 0;
            bulkApply(bleca, bufferThreshold, cons);
        }
        // FIXME: there is no medida historgram for this presently,
        // but maybe we would like one?
        TracyPlot("ledger.entry.commit", counter);

        // NB: we want to clear the prepared statement cache _before_
        // committing; on postgres this doesn't matter but on SQLite the passive
        // WAL-auto-checkpointing-at-commit behaviour will starve if there are
        // still prepared statements open at commit time.
        mDatabase.clearPreparedStatementCache();
        ZoneNamedN(commitZone, "SOCI commit", true);
        mTransaction->commit();
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
    mBestOffers.clear();
    mEntryCache.clear();

    // std::unique_ptr<...>::reset does not throw
    mTransaction.reset();

    // std::unique_ptr<...>::swap does not throw
    mHeader.swap(childHeader);
    mChild = nullptr;

    mPrefetchHits = 0;
    mPrefetchMisses = 0;
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
    case CLAIMABLE_BALANCE:
        return "claimablebalance";
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
                        " WHERE lastmodified >= :v1 AND lastmodified < :v2;";
    uint64_t count = 0;
    int first = static_cast<int>(ledgers.mFirst);
    int limit = static_cast<int>(ledgers.limit());
    mDatabase.getSession() << query, into(count), use(first), use(limit);
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
    mBestOffers.clear();

    for (auto let : {ACCOUNT, DATA, TRUSTLINE, OFFER, CLAIMABLE_BALANCE})
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

void
LedgerTxnRoot::dropClaimableBalances()
{
    mImpl->dropClaimableBalances();
}

uint32_t
LedgerTxnRoot::prefetch(UnorderedSet<LedgerKey> const& keys)
{
    return mImpl->prefetch(keys);
}

uint32_t
LedgerTxnRoot::Impl::prefetch(UnorderedSet<LedgerKey> const& keys)
{
    ZoneScoped;
    uint32_t total = 0;

    UnorderedSet<LedgerKey> accounts;
    UnorderedSet<LedgerKey> offers;
    UnorderedSet<LedgerKey> trustlines;
    UnorderedSet<LedgerKey> data;
    UnorderedSet<LedgerKey> claimablebalance;

    auto cacheResult =
        [&](UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>> const&
                res) {
            for (auto const& item : res)
            {
                putInEntryCache(item.first, item.second, LoadType::PREFETCH);
                ++total;
            }
        };

    auto insertIfNotLoaded = [&](UnorderedSet<LedgerKey>& keys,
                                 LedgerKey const& key) {
        if (!mEntryCache.exists(key, false))
        {
            keys.insert(key);
        }
    };

    for (auto const& key : keys)
    {
        switch (key.type())
        {
        case ACCOUNT:
            insertIfNotLoaded(accounts, key);
            if (accounts.size() == mBulkLoadBatchSize)
            {
                cacheResult(bulkLoadAccounts(accounts));
                accounts.clear();
            }
            break;
        case OFFER:
            insertIfNotLoaded(offers, key);
            if (offers.size() == mBulkLoadBatchSize)
            {
                cacheResult(bulkLoadOffers(offers));
                offers.clear();
            }
            break;
        case TRUSTLINE:
            insertIfNotLoaded(trustlines, key);
            if (trustlines.size() == mBulkLoadBatchSize)
            {
                cacheResult(bulkLoadTrustLines(trustlines));
                trustlines.clear();
            }
            break;
        case DATA:
            insertIfNotLoaded(data, key);
            if (data.size() == mBulkLoadBatchSize)
            {
                cacheResult(bulkLoadData(data));
                data.clear();
            }
            break;
        case CLAIMABLE_BALANCE:
            insertIfNotLoaded(claimablebalance, key);
            if (claimablebalance.size() == mBulkLoadBatchSize)
            {
                cacheResult(bulkLoadClaimableBalance(claimablebalance));
                claimablebalance.clear();
            }
            break;
        }
    }

    //  Prefetch whatever is remaining
    cacheResult(bulkLoadAccounts(accounts));
    cacheResult(bulkLoadOffers(offers));
    cacheResult(bulkLoadTrustLines(trustlines));
    cacheResult(bulkLoadData(data));
    cacheResult(bulkLoadClaimableBalance(claimablebalance));

    return total;
}

double
LedgerTxnRoot::getPrefetchHitRate() const
{
    return mImpl->getPrefetchHitRate();
}

double
LedgerTxnRoot::Impl::getPrefetchHitRate() const
{
    if (mPrefetchMisses == 0 && mPrefetchHits == 0)
    {
        return 0;
    }
    return static_cast<double>(mPrefetchHits) /
           (mPrefetchMisses + mPrefetchHits);
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxnRoot::getAllOffers()
{
    return mImpl->getAllOffers();
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxnRoot::Impl::getAllOffers()
{
    ZoneScoped;
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

    UnorderedMap<LedgerKey, LedgerEntry> offersByKey(offers.size());
    for (auto const& offer : offers)
    {
        offersByKey.emplace(LedgerEntryKey(offer), offer);
    }
    return offersByKey;
}

#ifdef BEST_OFFER_DEBUGGING
bool
LedgerTxnRoot::bestOfferDebuggingEnabled() const
{
    return mImpl->bestOfferDebuggingEnabled();
}

bool
LedgerTxnRoot::Impl::bestOfferDebuggingEnabled() const
{
    return mBestOfferDebuggingEnabled;
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::getBestOfferSlow(Asset const& buying, Asset const& selling,
                                OfferDescriptor const* worseThan,
                                std::unordered_set<int64_t>& exclude)
{
    return mImpl->getBestOfferSlow(buying, selling, worseThan, exclude);
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::getBestOfferSlow(Asset const& buying, Asset const& selling,
                                      OfferDescriptor const* worseThan,
                                      std::unordered_set<int64_t>& exclude)
{
    size_t const BATCH_SIZE = 1024;
    size_t nOffers = 0;
    std::deque<LedgerEntry> offers;
    do
    {
        nOffers += BATCH_SIZE;
        loadBestOffers(offers, buying, selling, nOffers);

        for (auto const& le : offers)
        {
            if (worseThan && !isBetterOffer(*worseThan, le))
            {
                continue;
            }

            if (exclude.find(le.data.offer().offerID) == exclude.end())
            {
                return std::make_shared<LedgerEntry const>(le);
            }
        }

        offers.clear();
    } while (offers.size() == nOffers);

    return nullptr;
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::checkBestOffer(Asset const& buying, Asset const& selling,
                                    OfferDescriptor const* worseThan,
                                    std::shared_ptr<LedgerEntry const> best)
{
    if (!bestOfferDebuggingEnabled())
    {
        return best;
    }

    std::unordered_set<int64_t> exclude;
    auto debugBest = getBestOfferSlow(buying, selling, worseThan, exclude);
    compareOffers(debugBest, best);
    return best;
}
#endif

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::getBestOffer(Asset const& buying, Asset const& selling)
{
#ifdef BEST_OFFER_DEBUGGING
    return mImpl->checkBestOffer(buying, selling, nullptr,
                                 mImpl->getBestOffer(buying, selling, nullptr));
#else
    return mImpl->getBestOffer(buying, selling, nullptr);
#endif
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::getBestOffer(Asset const& buying, Asset const& selling,
                            OfferDescriptor const& worseThan)
{
#ifdef BEST_OFFER_DEBUGGING
    return mImpl->checkBestOffer(
        buying, selling, &worseThan,
        mImpl->getBestOffer(buying, selling, &worseThan));
#else
    return mImpl->getBestOffer(buying, selling, &worseThan);
#endif
}

static std::deque<LedgerEntry>::const_iterator
findIncludedOffer(std::deque<LedgerEntry>::const_iterator iter,
                  std::deque<LedgerEntry>::const_iterator const& end,
                  OfferDescriptor const* worseThan)
{
    if (worseThan)
    {
        iter = std::upper_bound(
            iter, end, *worseThan,
            static_cast<bool (*)(OfferDescriptor const&, LedgerEntry const&)>(
                isBetterOffer));
    }
    return iter;
}

std::deque<LedgerEntry>::const_iterator
LedgerTxnRoot::Impl::loadNextBestOffersIntoCache(BestOffersEntryPtr cached,
                                                 Asset const& buying,
                                                 Asset const& selling)
{
    auto& offers = cached->bestOffers;
    if (cached->allLoaded)
    {
        return offers.cend();
    }

    size_t const BATCH_SIZE =
        std::min(mMaxBestOffersBatchSize,
                 std::max(MIN_BEST_OFFERS_BATCH_SIZE, offers.size()));

    std::deque<LedgerEntry>::const_iterator iter;
    try
    {
        if (offers.empty())
        {
            iter = loadBestOffers(offers, buying, selling, BATCH_SIZE);
        }
        else
        {
            auto const& oe = offers.back().data.offer();
            iter = loadBestOffers(offers, buying, selling,
                                  {oe.price, oe.offerID}, BATCH_SIZE);
        }
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

    cached->allLoaded =
        static_cast<size_t>(std::distance(iter, offers.cend())) < BATCH_SIZE;
    return iter;
}

void
LedgerTxnRoot::Impl::populateEntryCacheFromBestOffers(
    std::deque<LedgerEntry>::const_iterator iter,
    std::deque<LedgerEntry>::const_iterator const& end)
{
    UnorderedSet<LedgerKey> toPrefetch;
    for (; iter != end; ++iter)
    {
        auto const& oe = iter->data.offer();
        toPrefetch.emplace(accountKey(oe.sellerID));
        if (oe.buying.type() != ASSET_TYPE_NATIVE)
        {
            toPrefetch.emplace(trustlineKey(oe.sellerID, oe.buying));
        }
        if (oe.selling.type() != ASSET_TYPE_NATIVE)
        {
            toPrefetch.emplace(trustlineKey(oe.sellerID, oe.selling));
        }
    }
    prefetch(toPrefetch);
}

bool
LedgerTxnRoot::Impl::areEntriesMissingInCacheForOffer(OfferEntry const& oe)
{
    if (!mEntryCache.exists(accountKey(oe.sellerID)))
    {
        return true;
    }
    if (oe.buying.type() != ASSET_TYPE_NATIVE)
    {
        if (!mEntryCache.exists(trustlineKey(oe.sellerID, oe.buying)))
        {
            return true;
        }
    }
    if (oe.selling.type() != ASSET_TYPE_NATIVE)
    {
        if (!mEntryCache.exists(trustlineKey(oe.sellerID, oe.selling)))
        {
            return true;
        }
    }

    return false;
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::getBestOffer(Asset const& buying, Asset const& selling,
                                  OfferDescriptor const* worseThan)
{
    ZoneScoped;

    // Note: Elements of mBestOffers are properly sorted lists of the best
    // offers for a certain asset pair. This function maintaints the invariant
    // that the lists of best offers remain properly sorted. The sort order is
    // that determined by loadBestOffers and isBetterOffer (both induce the same
    // order).
    auto cached = getFromBestOffers(buying, selling);
    auto& offers = cached->bestOffers;

    // Batch-load best offers until an offer worse than *worseThan is found
    // (or until any offer is found if !worseThan)
    size_t initialBestOffersSize = offers.size();
    auto iter = findIncludedOffer(offers.cbegin(), offers.cend(), worseThan);
    while (iter == offers.cend() && !cached->allLoaded)
    {
        iter = loadNextBestOffersIntoCache(cached, buying, selling);
        iter = findIncludedOffer(iter, offers.cend(), worseThan);
    }

    bool newOffersLoaded = offers.size() != initialBestOffersSize;
    // Populate entry cache with upcoming best offers and prefetch associated
    // accounts and trust lines
    if (newOffersLoaded)
    {
        // At this point, we know that new offers were loaded. But new offers
        // will only be loaded if there were no offers worse than *worseThan
        // in the original list (or if the original list was empty if
        // !worseThan). In that case, iter must point into the newly loaded
        // offers so we will never try to prefetch the offers that had been
        // previously loaded.
        populateEntryCacheFromBestOffers(iter, offers.cend());
    }

    if (iter != offers.cend())
    {
        assert(!worseThan || isBetterOffer(*worseThan, *iter));

        // Check if we didn't prefetch and that we're missing
        // accounts/trustlines for this offer in the cache. If we are, batch
        // load for this offer and the next 999
        if (!newOffersLoaded &&
            areEntriesMissingInCacheForOffer(iter->data.offer()))
        {
            bool fullBatch =
                static_cast<size_t>(std::distance(iter, offers.cend())) >
                mMaxBestOffersBatchSize;
            auto lastOfferIter =
                fullBatch ? iter + mMaxBestOffersBatchSize : offers.cend();
            populateEntryCacheFromBestOffers(iter, lastOfferIter);
        }

        putInEntryCache(LedgerEntryKey(*iter),
                        std::make_shared<LedgerEntry const>(*iter),
                        LoadType::IMMEDIATE);

        return std::make_shared<LedgerEntry const>(*iter);
    }
    return nullptr;
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxnRoot::getOffersByAccountAndAsset(AccountID const& account,
                                          Asset const& asset)
{
    return mImpl->getOffersByAccountAndAsset(account, asset);
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxnRoot::Impl::getOffersByAccountAndAsset(AccountID const& account,
                                                Asset const& asset)
{
    ZoneScoped;
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

    UnorderedMap<LedgerKey, LedgerEntry> res(offers.size());
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

std::shared_ptr<InternalLedgerEntry const>
LedgerTxnRoot::getNewestVersion(InternalLedgerKey const& key) const
{
    return mImpl->getNewestVersion(key);
}

std::shared_ptr<InternalLedgerEntry const>
LedgerTxnRoot::Impl::getNewestVersion(InternalLedgerKey const& gkey) const
{
    ZoneScoped;
    // Right now, only LEDGER_ENTRY are recorded in the SQL database
    if (gkey.type() != InternalLedgerEntryType::LEDGER_ENTRY)
    {
        return nullptr;
    }
    auto const& key = gkey.ledgerKey();

    if (mEntryCache.exists(key))
    {
        std::string zoneTxt("hit");
        ZoneText(zoneTxt.c_str(), zoneTxt.size());
        return getFromEntryCache(key);
    }
    else
    {
        std::string zoneTxt("miss");
        ZoneText(zoneTxt.c_str(), zoneTxt.size());
        ++mPrefetchMisses;
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
        case CLAIMABLE_BALANCE:
            entry = loadClaimableBalance(key);
            break;
        default:
            throw std::runtime_error("Unknown key type");
        }
    }
    catch (NonSociRelatedException& e)
    {
        if (getHeader().ledgerVersion <= 14)
        {
            printErrorAndAbort(
                "fatal error when loading ledger entry from LedgerTxnRoot: ",
                e.what());
        }

        throw;
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

    putInEntryCache(key, entry, LoadType::IMMEDIATE);
    if (entry)
    {
        return std::make_shared<InternalLedgerEntry const>(*entry);
    }
    else
    {
        return nullptr;
    }
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
    mPrefetchHits = 0;
    mPrefetchMisses = 0;
}

std::shared_ptr<InternalLedgerEntry const>
LedgerTxnRoot::Impl::getFromEntryCache(LedgerKey const& key) const
{
    try
    {
        auto cached = mEntryCache.get(key);
        if (cached.type == LoadType::PREFETCH)
        {
            ++mPrefetchHits;
        }

        if (cached.entry)
        {
            return std::make_shared<InternalLedgerEntry const>(*cached.entry);
        }
        else
        {
            return nullptr;
        }
    }
    catch (...)
    {
        mEntryCache.clear();
        throw;
    }
}

void
LedgerTxnRoot::Impl::putInEntryCache(
    LedgerKey const& key, std::shared_ptr<LedgerEntry const> const& entry,
    LoadType type) const
{
    try
    {
        mEntryCache.put(key, {entry, type});
    }
    catch (...)
    {
        mEntryCache.clear();
        throw;
    }
}

LedgerTxnRoot::Impl::BestOffersEntryPtr
LedgerTxnRoot::Impl::getFromBestOffers(Asset const& buying,
                                       Asset const& selling) const
{
    try
    {
        BestOffersKey offersKey{buying, selling};
        auto it = mBestOffers.find(offersKey);

        if (it != mBestOffers.end())
        {
            return it->second;
        }

        auto emptyPtr =
            std::make_shared<BestOffersEntry>(BestOffersEntry{{}, false});
        mBestOffers.emplace(offersKey, emptyPtr);
        return emptyPtr;
    }
    catch (...)
    {
        mBestOffers.clear();
        throw;
    }
}
}
