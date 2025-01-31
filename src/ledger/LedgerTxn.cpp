// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "bucket/BucketManager.h"
#include "bucket/SearchableBucketList.h"
#include "crypto/KeyUtils.h"
#include "database/Database.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NonSociRelatedException.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <Tracy.hpp>
#include <soci.h>

#include <algorithm>
#include <stdexcept>

namespace stellar
{

LedgerEntryPtr
LedgerEntryPtr::Init(std::shared_ptr<InternalLedgerEntry> const& lePtr)
{
    return {lePtr, EntryPtrState::INIT};
}

LedgerEntryPtr
LedgerEntryPtr::Live(std::shared_ptr<InternalLedgerEntry> const& lePtr)
{
    return {lePtr, EntryPtrState::LIVE};
}

LedgerEntryPtr
LedgerEntryPtr::Delete()
{
    return {nullptr, EntryPtrState::DELETED};
}

LedgerEntryPtr::LedgerEntryPtr(
    std::shared_ptr<InternalLedgerEntry> const& lePtr, EntryPtrState state)
    : mEntryPtr(lePtr), mState(state)
{
    if (lePtr)
    {
        if (isDeleted())
        {
            throw std::runtime_error("DELETED LedgerEntryPtr is not null");
        }
    }
    else
    {
        if (isInit() || isLive())
        {
            throw std::runtime_error("INIT/LIVE LedgerEntryPtr is null");
        }
    }
}

InternalLedgerEntry&
LedgerEntryPtr::operator*() const
{
    if (!mEntryPtr)
    {
        throw std::runtime_error("cannot dereference null mEntryPtr");
    }

    return *mEntryPtr;
}

InternalLedgerEntry*
LedgerEntryPtr::operator->() const
{
    if (!mEntryPtr)
    {
        throw std::runtime_error("cannot dereference null mEntryPtr");
    }

    return mEntryPtr.get();
}

std::shared_ptr<InternalLedgerEntry>
LedgerEntryPtr::get() const
{
    return mEntryPtr;
}

void
LedgerEntryPtr::mergeFrom(LedgerEntryPtr const& entryPtr)
{
    switch (mState)
    {
    case EntryPtrState::INIT:
    {
        if (entryPtr.isDeleted())
        {
            // This isn't possible because we don't call mergeFrom in this case.
            // Instead, the init entry is annihilated by the delete
            throw std::runtime_error("cannot delete non-live entry");
        }
    }
    break;
    case EntryPtrState::LIVE:
    {
        // cannot commit an init entry into a live entry (If the parent entry is
        // live, the child could not have created the same entry)
        if (entryPtr.isInit())
        {
            throw std::runtime_error(
                "cannot commit a child init entry into a parent live entry");
        }

        // propagate state
        mState = entryPtr.getState();
    }
    break;
    case EntryPtrState::DELETED:
    {
        switch (entryPtr.getState())
        {
        case EntryPtrState::INIT:
            mState = EntryPtrState::LIVE;
            break;
        case EntryPtrState::LIVE:
            throw std::runtime_error("cannot set deleted entry to live");
        case EntryPtrState::DELETED:
            throw std::runtime_error("cannot delete deleted entry");
        }
        break;
    }
    default:
        throw std::runtime_error("unknown EntryPtrState");
    }

    // std::shared_ptr<...>::operator= does not throw
    mEntryPtr = entryPtr.get();
}

EntryPtrState
LedgerEntryPtr::getState() const
{
    return mState;
}

bool
LedgerEntryPtr::isInit() const
{
    return mState == EntryPtrState::INIT;
}

bool
LedgerEntryPtr::isLive() const
{
    return mState == EntryPtrState::LIVE;
}

bool
LedgerEntryPtr::isDeleted() const
{
    return mState == EntryPtrState::DELETED;
}
bool
LedgerKeyMeter::canLoad(LedgerKey const& key, size_t entrySizeBytes) const
{
    return maxReadQuotaForKey(key) >= entrySizeBytes;
}

void
LedgerKeyMeter::addTxn(SorobanResources const& resources)
{
    TxReadBytesPtr txReadBytesPtr =
        std::make_shared<uint32_t>(resources.readBytes);
    auto addKeyToTxnMap = [&](auto const& key) {
        mLedgerKeyToTxReadBytes[key].emplace_back(txReadBytesPtr);
    };
    std::for_each(resources.footprint.readOnly.begin(),
                  resources.footprint.readOnly.end(), addKeyToTxnMap);
    std::for_each(resources.footprint.readWrite.begin(),
                  resources.footprint.readWrite.end(), addKeyToTxnMap);
}

void
LedgerKeyMeter::updateReadQuotasForKey(LedgerKey const& key,
                                       size_t entrySizeBytes)
{
    auto iter = mLedgerKeyToTxReadBytes.find(key);
    if (iter == mLedgerKeyToTxReadBytes.end())
    {
        // Key does not belong to the footprint of any transaction.
        // Ensure this is not a soroban key as they should always be metered.
        releaseAssert(key.type() != CONTRACT_CODE &&
                      key.type() != CONTRACT_DATA);
        return;
    }
    // Update the read quota for every transaction containing this key.
    bool exceedsQuotaForAllTxns = true;
    for (TxReadBytesPtr txReadBytesPtr : iter->second)
    {
        if (*txReadBytesPtr < entrySizeBytes)
        {
            *txReadBytesPtr = 0;
        }
        else
        {
            exceedsQuotaForAllTxns = false;
            *txReadBytesPtr -= entrySizeBytes;
        }
    }
    if (exceedsQuotaForAllTxns)
    {
        mNotLoadedKeys.insert(key);
    }
}

uint32_t
LedgerKeyMeter::maxReadQuotaForKey(LedgerKey const& key) const
{
    auto iter = mLedgerKeyToTxReadBytes.find(key);
    if (iter == mLedgerKeyToTxReadBytes.end())
    {
        // Key does not belong to the footprint of any transaction,
        // therefore it is not quota-limited.
        // Ensure this is not a soroban key as they should always be metered.
        releaseAssert(key.type() != CONTRACT_CODE &&
                      key.type() != CONTRACT_DATA);
        return std::numeric_limits<uint32_t>::max();
    }
    return **std::max_element(
        iter->second.begin(), iter->second.end(),
        [&](TxReadBytesPtr a, TxReadBytesPtr b) { return *a < *b; });
}

bool
LedgerKeyMeter::loadFailed(LedgerKey const& key) const
{
    if (mNotLoadedKeys.find(key) != mNotLoadedKeys.end())
    {
        return true;
    }
    return false;
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

LedgerEntryPtr const&
EntryIterator::entryPtr() const
{
    return getImpl()->entryPtr();
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

// Implementation of AbstractLedgerTxn --------------------------------------
AbstractLedgerTxn::~AbstractLedgerTxn()
{
}

// Implementation of LedgerTxn ----------------------------------------------
LedgerTxn::LedgerTxn(AbstractLedgerTxnParent& parent,
                     bool shouldUpdateLastModified, TransactionMode mode)
    : mImpl(
          std::make_unique<Impl>(*this, parent, shouldUpdateLastModified, mode))
{
}

LedgerTxn::LedgerTxn(LedgerTxn& parent, bool shouldUpdateLastModified,
                     TransactionMode mode)
    : LedgerTxn((AbstractLedgerTxnParent&)parent, shouldUpdateLastModified,
                mode)
{
}

LedgerTxn::Impl::Impl(LedgerTxn& self, AbstractLedgerTxnParent& parent,
                      bool shouldUpdateLastModified, TransactionMode mode)
    : mParent(parent)
    , mChild(nullptr)
    , mHeader(std::make_unique<LedgerHeader>(mParent.getHeader()))
    , mShouldUpdateLastModified(shouldUpdateLastModified)
    , mIsSealed(false)
    , mConsistency(LedgerTxnConsistency::EXACT)
{
    mParent.addChild(self, mode);
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
LedgerTxn::addChild(AbstractLedgerTxn& child, TransactionMode mode)
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
LedgerTxn::Impl::throwIfErasingConfig(InternalLedgerKey const& key) const
{
    if (key.type() == InternalLedgerEntryType::LEDGER_ENTRY &&
        key.ledgerKey().type() == CONFIG_SETTING)
    {
        throw std::runtime_error("Configuration settings cannot be erased.");
    }
}

void
LedgerTxn::commit() noexcept
{
    getImpl()->commit();
    mImpl.reset();
}

void
LedgerTxn::Impl::commit() noexcept
{
    maybeUpdateLastModifiedThenInvokeThenSeal([&](EntryMap const& entries) {
        // getEntryIterator has the strong exception safety guarantee
        // commitChild has the strong exception safety guarantee
        mParent.commitChild(getEntryIterator(entries), mRestoredKeys,
                            mConsistency);
    });
}

void
LedgerTxn::commitChild(EntryIterator iter, RestoredKeys const& restoredKeys,
                       LedgerTxnConsistency cons) noexcept
{
    getImpl()->commitChild(std::move(iter), restoredKeys, cons);
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
LedgerTxn::Impl::commitChild(EntryIterator iter,
                             RestoredKeys const& restoredKeys,
                             LedgerTxnConsistency cons) noexcept
{
    // Assignment of xdrpp objects does not have the strong exception safety
    // guarantee, so use std::unique_ptr<...>::swap to achieve it
    auto childHeader = std::make_unique<LedgerHeader>(mChild->getHeader());

    mConsistency = joinConsistencyLevels(mConsistency, cons);

    if (!mActive.empty())
    {
        printErrorAndAbort(
            "Attempting to commit a child while parent has active entries");
    }
    try
    {
        for (; (bool)iter; ++iter)
        {
            updateEntry(iter.key(), /* keyHint */ nullptr, iter.entryPtr(),
                        /* effectiveActive */ false);
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
        mChild->forAllWorstBestOffers(
            [&](Asset const& buying, Asset const& selling,
                std::shared_ptr<OfferDescriptor const>& desc) {
                updateWorstBestOffer(AssetPair{buying, selling}, desc);
            });
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

    for (auto const& key : restoredKeys.hotArchive)
    {
        auto [_, inserted] = mRestoredKeys.hotArchive.emplace(key);
        if (!inserted)
        {
            printErrorAndAbort("restored hot archive entry already exists");
        }
    }

    for (auto const& key : restoredKeys.liveBucketList)
    {
        auto [_, inserted] = mRestoredKeys.liveBucketList.emplace(key);
        if (!inserted)
        {
            printErrorAndAbort("restored live BucketList entry already exists");
        }
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

    auto it = mEntry.end(); // hint that key is not in mEntry

    // If the key currently exists in mEntry as a DELETED entry, the new state
    // after this INIT entry is merged with the DELETED will be a LIVE. This is
    // because the entry would have been a LIVE before the delete. If it were an
    // INIT instead, the key would've been annihilated.
    updateEntry(key, &it, LedgerEntryPtr::Init(current),
                /* effectiveActive */ true);
    return ltxe;
}

void
LedgerTxn::createWithoutLoading(InternalLedgerEntry const& entry)
{
    getImpl()->createWithoutLoading(entry);
}

void
LedgerTxn::Impl::createWithoutLoading(InternalLedgerEntry const& entry)
{
    throwIfSealed();
    throwIfChild();

    auto key = entry.toKey();
    auto iter = mActive.find(key);
    if (iter != mActive.end())
    {
        throw std::runtime_error("Key is already active");
    }

    // If the key currently exists in mEntry as a DELETED entry, the new state
    // after this INIT entry is merged with the DELETED will be a LIVE. This is
    // because the entry would have been a LIVE before the delete. If it were an
    // INIT instead, the key would've been annihilated.
    updateEntry(
        key, /* keyHint */ nullptr,
        LedgerEntryPtr::Init(std::make_shared<InternalLedgerEntry>(entry)),
        /* effectiveActive */ false);
}

void
LedgerTxn::updateWithoutLoading(InternalLedgerEntry const& entry)
{
    getImpl()->updateWithoutLoading(entry);
}

void
LedgerTxn::Impl::updateWithoutLoading(InternalLedgerEntry const& entry)
{
    throwIfSealed();
    throwIfChild();

    auto key = entry.toKey();
    auto iter = mActive.find(key);
    if (iter != mActive.end())
    {
        throw std::runtime_error("Key is already active");
    }

    updateEntry(
        key, /* keyHint */ nullptr,
        LedgerEntryPtr::Live(std::make_shared<InternalLedgerEntry>(entry)),
        /* effectiveActive */ false);
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

    auto newest = getNewestVersionEntryMap(key);
    if (!newest.first)
    {
        throw std::runtime_error("Key does not exist");
    }
    throwIfErasingConfig(key);

    auto activeIter = mActive.find(key);
    bool isActive = activeIter != mActive.end();

    updateEntry(key, &newest.second, LedgerEntryPtr::Delete(), false);
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
LedgerTxn::restoreFromHotArchive(LedgerEntry const& entry, uint32_t ttl)
{
    getImpl()->restoreFromHotArchive(*this, entry, ttl);
}

void
LedgerTxn::Impl::restoreFromHotArchive(LedgerTxn& self,
                                       LedgerEntry const& entry, uint32_t ttl)
{
    throwIfSealed();
    throwIfChild();

    if (!isPersistentEntry(entry.data))
    {
        throw std::runtime_error("Key type not supported in Hot Archive");
    }
    auto ttlKey = getTTLKey(entry);

    // Restore entry by creating it on the live BucketList
    create(self, entry);

    // Also create the corresponding TTL entry
    LedgerEntry ttlEntry;
    ttlEntry.data.type(TTL);
    ttlEntry.data.ttl().liveUntilLedgerSeq = ttl;
    ttlEntry.data.ttl().keyHash = ttlKey.ttl().keyHash;
    create(self, ttlEntry);

    // Mark the keys as restored
    auto addKey = [this](LedgerKey const& key) {
        auto [_, inserted] = mRestoredKeys.hotArchive.insert(key);
        if (!inserted)
        {
            throw std::runtime_error("Key already removed from hot archive");
        }
    };
    addKey(LedgerEntryKey(entry));
    addKey(ttlKey);
}

void
LedgerTxn::restoreFromLiveBucketList(LedgerKey const& key, uint32_t ttl)
{
    getImpl()->restoreFromLiveBucketList(*this, key, ttl);
}

void
LedgerTxn::Impl::restoreFromLiveBucketList(LedgerTxn& self,
                                           LedgerKey const& key, uint32_t ttl)
{
    throwIfSealed();
    throwIfChild();

    if (!isPersistentEntry(key))
    {
        throw std::runtime_error("Key type not supported for restoration");
    }

    auto ttlKey = getTTLKey(key);

    // Note: key should have already been loaded via loadWithoutRecord by
    // caller, so this read should already be in the cache.
    auto ttlLtxe = load(self, ttlKey);
    if (!ttlLtxe)
    {
        throw std::runtime_error("Entry restored from live BucketList but does "
                                 "not exist in the live BucketList.");
    }

    ttlLtxe.current().data.ttl().liveUntilLedgerSeq = ttl;

    // Mark the keys as restored
    auto addKey = [this](LedgerKey const& key) {
        auto [_, inserted] = mRestoredKeys.liveBucketList.insert(key);
        if (!inserted)
        {
            throw std::runtime_error(
                "Key already restored from Live BucketList");
        }
    };
    addKey(key);
    addKey(ttlKey);
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
    throwIfErasingConfig(key);

    auto activeIter = mActive.find(key);
    bool isActive = activeIter != mActive.end();

    updateEntry(key, /* keyHint */ nullptr, LedgerEntryPtr::Delete(), false);
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
        if (entry.isDeleted())
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

        if (!kv.second.isDeleted())
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
                selfBest = kv.second.get();
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
    auto ob = findOrderBook(buying, selling);
    if (ob)
    {
        auto& offers = *ob;
        if (!offers.empty())
        {
            auto entryIter = mEntry.find(offers.begin()->second);
            if (entryIter == mEntry.end() || entryIter->second.isDeleted())
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
    auto ob = findOrderBook(buying, selling);
    if (ob)
    {
        auto const& offers = *ob;
        auto iter = offers.upper_bound(worseThan);
        if (iter != offers.end())
        {
            auto entryIter = mEntry.find(iter->second);
            if (entryIter == mEntry.end() || entryIter->second.isDeleted())
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

            if (entry.isInit())
            {
                changes.emplace_back(LEDGER_ENTRY_CREATED);
                changes.back().created() = entry->ledgerEntry();
            }
            else
            {
                auto previous = mParent.getNewestVersion(key);
                // entry is not init, so previous must exist. If not, then
                // we're modifying an entry that doesn't exist.
                releaseAssert(previous);

                changes.emplace_back(LEDGER_ENTRY_STATE);
                changes.back().state() = previous->ledgerEntry();

                if (entry.isDeleted())
                {
                    changes.emplace_back(LEDGER_ENTRY_REMOVED);
                    changes.back().removed() = key.ledgerKey();
                }
                else
                {
                    changes.emplace_back(LEDGER_ENTRY_UPDATED);
                    changes.back().updated() = entry->ledgerEntry();
                }
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
            if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY)
            {
                continue;
            }

            auto previous = mParent.getNewestVersion(key);

            // Deep copy is not required here because getDelta causes
            // LedgerTxn to enter the sealed state, meaning subsequent
            // modifications are impossible.
            delta.entry[key] = {kv.second.get(), previous};
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

        if (!entry.isDeleted())
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

            if (entry.get())
            {
                if (entry.isInit())
                {
                    resInit.emplace_back(entry->ledgerEntry());
                }
                else
                {
                    resLive.emplace_back(entry->ledgerEntry());
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

UnorderedSet<LedgerKey> const&
LedgerTxn::getRestoredHotArchiveKeys() const
{
    return getImpl()->getRestoredHotArchiveKeys();
}

UnorderedSet<LedgerKey> const&
LedgerTxn::Impl::getRestoredHotArchiveKeys() const
{
    return mRestoredKeys.hotArchive;
}

UnorderedSet<LedgerKey> const&
LedgerTxn::getRestoredLiveBucketListKeys() const
{
    return getImpl()->getRestoredLiveBucketListKeys();
}

UnorderedSet<LedgerKey> const&
LedgerTxn::Impl::getRestoredLiveBucketListKeys() const
{
    return mRestoredKeys.liveBucketList;
}

LedgerKeySet
LedgerTxn::getAllTTLKeysWithoutSealing() const
{
    return getImpl()->getAllTTLKeysWithoutSealing();
}

LedgerKeySet
LedgerTxn::Impl::getAllTTLKeysWithoutSealing() const
{
    throwIfNotExactConsistency();
    LedgerKeySet result;
    for (auto const& [k, v] : mEntry)
    {
        if (k.type() == InternalLedgerEntryType::LEDGER_ENTRY &&
            k.ledgerKey().type() == TTL)
        {
            result.emplace(k.ledgerKey());
        }
    }

    return result;
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
        return iter->second.get();
    }
    return mParent.getNewestVersion(key);
}

std::pair<std::shared_ptr<InternalLedgerEntry const>,
          LedgerTxn::Impl::EntryMap::iterator>
LedgerTxn::Impl::getNewestVersionEntryMap(InternalLedgerKey const& key)
{
    auto iter = mEntry.find(key);
    if (iter != mEntry.end())
    {
        return std::make_pair(iter->second.get(), iter);
    }
    return std::make_pair(mParent.getNewestVersion(key), iter);
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
        if (entry.isDeleted())
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

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxn::getPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                                   Asset const& asset)
{
    return getImpl()->getPoolShareTrustLinesByAccountAndAsset(account, asset);
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxn::Impl::getPoolShareTrustLinesByAccountAndAsset(
    AccountID const& account, Asset const& asset)
{
    auto trustLines =
        mParent.getPoolShareTrustLinesByAccountAndAsset(account, asset);
    for (auto const& kv : mEntry)
    {
        if (kv.first.type() != InternalLedgerEntryType::LEDGER_ENTRY)
        {
            continue;
        }

        auto const& key = kv.first.ledgerKey();
        if (key.type() == TRUSTLINE && key.trustLine().accountID == account &&
            key.trustLine().asset.type() == ASSET_TYPE_POOL_SHARE)
        {
            if (kv.second.isDeleted())
            {
                // The trust line was in our result set from a parent, but was
                // deleted in self
                trustLines.erase(key);
            }
            else
            {
                auto iter = trustLines.find(key);
                if (iter != trustLines.end())
                {
                    // The trust line was in our result set from a parent, and
                    // was updated in self
                    iter->second = kv.second->ledgerEntry();
                }
                else
                {
                    // The trust line wasn't in our result set, and was updated
                    // in self. We need to check the corresponding LiquidityPool
                    // to find its constituent assets.
                    auto newest = getNewestVersion(liquidityPoolKey(
                        key.trustLine().asset.liquidityPoolID()));
                    if (!newest)
                    {
                        throw std::runtime_error("Invalid ledger state");
                    }

                    auto const& lp = newest->ledgerEntry().data.liquidityPool();
                    auto const& cp = lp.body.constantProduct();
                    if (cp.params.assetA == asset || cp.params.assetB == asset)
                    {
                        trustLines.emplace(key, kv.second->ledgerEntry());
                    }
                }
            }
        }
    }
    return trustLines;
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

    auto newest = getNewestVersionEntryMap(key);
    if (!newest.first)
    {
        return {};
    }

    std::optional<LedgerEntryPtr> currentEntryPtr;
    if (newest.second != mEntry.end())
    {
        currentEntryPtr = std::optional<LedgerEntryPtr>(newest.second->second);
    }
    else
    {
        currentEntryPtr = LedgerEntryPtr::Live(
            std::make_shared<InternalLedgerEntry>(*newest.first));
    }

    releaseAssert(currentEntryPtr.has_value());
    auto impl = LedgerTxnEntry::makeSharedImpl(self, *currentEntryPtr->get());

    // Set the key to active before constructing the LedgerTxnEntry, as this
    // can throw and the LedgerTxnEntry destructor requires that mActive
    // contains key. LedgerTxnEntry constructor does not throw so this is
    // still exception safe.
    mActive.emplace(key, toEntryImplBase(impl));
    LedgerTxnEntry ltxe(impl);

    // If this throws, the order book will not be modified because of the strong
    // exception safety guarantee. Furthermore, ltxe will be destructed leading
    // to key being deactivated. This will leave LedgerTxn unmodified.
    updateEntry(key, &newest.second, *currentEntryPtr,
                /* effectiveActive */ true);
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

std::vector<LedgerTxnEntry>
LedgerTxn::loadPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                                    Asset const& asset)
{
    return getImpl()->loadPoolShareTrustLinesByAccountAndAsset(*this, account,
                                                               asset);
}

std::vector<LedgerTxnEntry>
LedgerTxn::Impl::loadPoolShareTrustLinesByAccountAndAsset(
    LedgerTxn& self, AccountID const& account, Asset const& asset)
{
    throwIfSealed();
    throwIfChild();

    auto previousEntries = mEntry;
    auto trustLines = getPoolShareTrustLinesByAccountAndAsset(account, asset);
    try
    {
        std::vector<LedgerTxnEntry> res;
        res.reserve(trustLines.size());
        for (auto const& kv : trustLines)
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
LedgerTxn::rollback() noexcept
{
    getImpl()->rollback();
    mImpl.reset();
}

void
LedgerTxn::Impl::rollback() noexcept
{
    if (mChild)
    {
        mChild->rollback();
    }

    mEntry.clear();
    mRestoredKeys.hotArchive.clear();
    mRestoredKeys.liveBucketList.clear();
    mMultiOrderBook.clear();
    mActive.clear();
    mActiveHeader.reset();
    mIsSealed = true;

    mParent.rollbackChild();
}

void
LedgerTxn::rollbackChild() noexcept
{
    getImpl()->rollbackChild();
}

void
LedgerTxn::Impl::rollbackChild() noexcept
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
LedgerTxn::countOffers(LedgerRange const& ledgers) const
{
    throw std::runtime_error("called countOffers on non-root LedgerTxn");
}

void
LedgerTxn::deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const
{
    throw std::runtime_error(
        "called deleteOffersModifiedOnOrAfterLedger on non-root LedgerTxn");
}

void
LedgerTxn::dropOffers()
{
    throw std::runtime_error("called dropOffers on non-root LedgerTxn");
}

double
LedgerTxn::getPrefetchHitRate() const
{
    return getImpl()->getPrefetchHitRate();
}

#ifdef BUILD_TESTS
void
LedgerTxn::resetForFuzzer()
{
    abort();
}
#endif // BUILD_TESTS

double
LedgerTxn::Impl::getPrefetchHitRate() const
{
    return mParent.getPrefetchHitRate();
}

uint32_t
LedgerTxn::prefetchClassic(UnorderedSet<LedgerKey> const& keys)
{
    return getImpl()->prefetchClassic(keys);
}
uint32_t
LedgerTxn::prefetchSoroban(UnorderedSet<LedgerKey> const& keys,
                           LedgerKeyMeter* lkMeter)
{
    return getImpl()->prefetchSoroban(keys, lkMeter);
}

uint32_t
LedgerTxn::Impl::prefetchClassic(UnorderedSet<LedgerKey> const& keys)
{
    return mParent.prefetchClassic(keys);
}

uint32_t
LedgerTxn::Impl::prefetchSoroban(UnorderedSet<LedgerKey> const& keys,
                                 LedgerKeyMeter* lkMeter)
{
    return mParent.prefetchSoroban(keys, lkMeter);
}

void
LedgerTxn::Impl::maybeUpdateLastModified() noexcept
{
    throwIfSealed();
    throwIfChild();

    for (auto& kv : mEntry)
    {
        auto& entry = kv.second;
        if (!kv.second.isDeleted())
        {
            if (mShouldUpdateLastModified &&
                entry->type() == InternalLedgerEntryType::LEDGER_ENTRY)
            {
                entry->ledgerEntry().lastModifiedLedgerSeq = mHeader->ledgerSeq;
            }
        }
    }
}

void
LedgerTxn::Impl::maybeUpdateLastModifiedThenInvokeThenSeal(
    std::function<void(EntryMap const&)> f) noexcept
{
    if (!mIsSealed)
    {
        maybeUpdateLastModified();

        f(mEntry);

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

void
LedgerTxn::Impl::removeFromOrderBookIfExists(LedgerEntry const& le)
{
    auto const& oe = le.data.offer();
    auto mobIterBuying = mMultiOrderBook.find(oe.buying);
    if (mobIterBuying != mMultiOrderBook.end())
    {
        auto& mobBuying = mobIterBuying->second;
        auto mobIterSelling = mobBuying.find(oe.selling);
        if (mobIterSelling != mobBuying.end())
        {
            auto& mobSelling = mobIterSelling->second;
            auto obIter = mobSelling.find({oe.price, oe.offerID});
            if (obIter != mobSelling.end())
            {
                mobSelling.erase(obIter);

                if (mobSelling.empty())
                {
                    mobBuying.erase(mobIterSelling);
                    if (mobBuying.empty())
                    {
                        mMultiOrderBook.erase(mobIterBuying);
                    }
                }
            }
        }
    }
}

LedgerTxn::Impl::OrderBook*
LedgerTxn::Impl::findOrderBook(Asset const& buying, Asset const& selling)
{
    auto mobIterBuying = mMultiOrderBook.find(buying);
    if (mobIterBuying != mMultiOrderBook.end())
    {
        auto& mobBuying = mobIterBuying->second;
        auto mobIterSelling = mobBuying.find(selling);
        if (mobIterSelling != mobBuying.end())
        {
            auto& mobSelling = mobIterSelling->second;
            return &mobSelling;
        }
    }
    return nullptr;
}

void
LedgerTxn::Impl::updateEntryIfRecorded(InternalLedgerKey const& key,
                                       bool effectiveActive)
{
    // This early return is just an optimization. updateEntryIfRecorded does not
    // end up modifying mEntry because it loads the entry here, and then
    // attempts to update that same entry in updateEntry using the identical one
    // that was loaded. It just refreshes mMultiOrderBook in updateEntry, so
    // there's nothing to do if the entry is not an offer. If updateEntry is
    // updated in the future to maintain additional state outside of mEntry,
    // this optimization might have to be modified.
    if (key.type() != InternalLedgerEntryType::LEDGER_ENTRY ||
        key.ledgerKey().type() != OFFER)
    {
        return;
    }

    auto entryIter = mEntry.find(key);
    // If loadWithoutRecord was used, then key may not be in mEntry. But if key
    // is not in mEntry, then there is no reason to have an entry in the order
    // book. Therefore we only updateEntry if key is in mEntry.
    if (entryIter != mEntry.end())
    {
        updateEntry(key, &entryIter, entryIter->second, effectiveActive);
    }
}

void
LedgerTxn::Impl::updateEntry(InternalLedgerKey const& key,
                             EntryMap::iterator const* keyHint,
                             LedgerEntryPtr lePtr,
                             bool effectiveActive) noexcept
{
    auto recordEntry = [&]() {
        // First, try to insert the entry. If the entry doesn't already exist,
        // then mEntry just accepts the state of this new entry and there's
        // nothing else to do. However, if the key exists, then we either update
        // the old entry using the new entry, or erase the key if the existing
        // entry is a init and the update is a delete.
        bool inserted = false;
        EntryMap::iterator localIterDoNotUse;
        if (!keyHint || *keyHint == mEntry.end())
        {
            std::tie(localIterDoNotUse, inserted) = mEntry.emplace(key, lePtr);
            keyHint = &localIterDoNotUse;
        }

        if (!inserted)
        {
            if (!keyHint || *keyHint == mEntry.end())
            {
                throw std::runtime_error("invalid keyHint state");
            }

            // An init entry is being deleted, so annihilate the init instead of
            // updating it.
            if (lePtr.isDeleted() && (*keyHint)->second.isInit())
            {
                mEntry.erase(*keyHint);
            }
            else
            {
                (*keyHint)->second.mergeFrom(lePtr);
            }
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

    // this iterator should not be used directly: use keyHint instead
    EntryMap::iterator localIterDoNotUse;
    if (!keyHint)
    {
        localIterDoNotUse = mEntry.find(key);
        keyHint = &localIterDoNotUse;
    }
    if (*keyHint != mEntry.end() && !(*keyHint)->second.isDeleted())
    {
        // The offer is always removed from mMultiOrderBook even if this is a
        // modification because the assets on the existing offer can be modified
        auto const& le = (*keyHint)->second->ledgerEntry();
        removeFromOrderBookIfExists(le);
    }

    // We only insert the new offer into the order book if it exists and is not
    // active. Otherwise, we just record the update in mEntry and return.
    if (!lePtr.isDeleted() && !effectiveActive)
    {
        auto const& oe = lePtr->ledgerEntry().data.offer();

        auto& ob = mMultiOrderBook[oe.buying][oe.selling];
        ob.emplace(OfferDescriptor{oe.price, oe.offerID}, key.ledgerKey());
    }
    recordEntry();
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

void
LedgerTxn::forAllWorstBestOffers(WorstOfferProcessor proc)
{
    getImpl()->forAllWorstBestOffers(proc);
}

void
LedgerTxn::Impl::forAllWorstBestOffers(WorstOfferProcessor proc)
{
    for (auto& wo : mWorstBestOffer)
    {
        auto& ap = wo.first;
        proc(ap.buying, ap.selling, wo.second);
    }
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

SessionWrapper&
LedgerTxn::getSession() const
{
    throw std::runtime_error("LedgerTxn::getSession illegal call, can only be "
                             "called on LedgerTxnRoot");
}

void
LedgerTxn::prepareNewObjects(size_t s)
{
    getImpl()->prepareNewObjects(s);
}

void
LedgerTxn::Impl::prepareNewObjects(size_t s)
{
    size_t newSize = mEntry.size();
    auto constexpr m = std::numeric_limits<size_t>::max();
    if (newSize >= m - s)
    {
        newSize = m;
    }
    else
    {
        newSize += s;
    }
    mEntry.reserve(newSize);
}

#ifdef BUILD_TESTS
UnorderedMap<AssetPair,
             std::map<OfferDescriptor, LedgerKey, IsBetterOfferComparator>,
             AssetPairHash>
LedgerTxn::getOrderBook() const
{
    return getImpl()->getOrderBook();
}

UnorderedMap<AssetPair,
             std::map<OfferDescriptor, LedgerKey, IsBetterOfferComparator>,
             AssetPairHash>
LedgerTxn::Impl::getOrderBook() const
{
    UnorderedMap<AssetPair,
                 std::map<OfferDescriptor, LedgerKey, IsBetterOfferComparator>,
                 AssetPairHash>
        res;
    for (auto& b : mMultiOrderBook)
    {
        for (auto& s : b.second)
        {
            AssetPair p{b.first, s.first};
            res[p].insert(s.second.begin(), s.second.end());
        }
    }
    return res;
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

LedgerEntryPtr const&
LedgerTxn::Impl::EntryIteratorImpl::entryPtr() const
{
    return mIter->second;
}

bool
LedgerTxn::Impl::EntryIteratorImpl::entryExists() const
{
    return !mIter->second.isDeleted();
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

// Implementation of LedgerTxnRoot ------------------------------------------
size_t const LedgerTxnRoot::Impl::MIN_BEST_OFFERS_BATCH_SIZE = 5;

LedgerTxnRoot::LedgerTxnRoot(Application& app, size_t entryCacheSize,
                             size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                             ,
                             bool bestOfferDebuggingEnabled
#endif
                             )
    : mImpl(std::make_unique<Impl>(app, entryCacheSize, prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                                   ,
                                   bestOfferDebuggingEnabled
#endif
                                   ))
{
}

LedgerTxnRoot::Impl::Impl(Application& app, size_t entryCacheSize,
                          size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                          ,
                          bool bestOfferDebuggingEnabled
#endif
                          )
    : mMaxBestOffersBatchSize(
          std::min(std::max(prefetchBatchSize, MIN_BEST_OFFERS_BATCH_SIZE),
                   getMaxOffersToCross()))
    , mApp(app)
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

SessionWrapper&
LedgerTxnRoot::Impl::getSession() const
{
    if (mSession)
    {
        return *mSession;
    }
    return mApp.getDatabase().getSession();
}

SessionWrapper&
LedgerTxnRoot::getSession() const
{
    return mImpl->getSession();
}

#ifdef BUILD_TESTS
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
#endif // BUILD_TESTS

void
LedgerTxnRoot::addChild(AbstractLedgerTxn& child, TransactionMode mode)
{
    mImpl->addChild(child, mode);
}

void
LedgerTxnRoot::Impl::addChild(AbstractLedgerTxn& child, TransactionMode mode)
{
    if (mChild)
    {
        throw std::runtime_error("LedgerTxnRoot already has child");
    }

    if (mode == TransactionMode::READ_WRITE_WITH_SQL_TXN)
    {
        if (mApp.getConfig().parallelLedgerClose())
        {
            mSession = std::make_unique<SessionWrapper>(
                "ledgerClose", mApp.getDatabase().getPool());
        }
        mTransaction =
            std::make_unique<soci::transaction>(getSession().session());
    }
    else
    {
        // Read-only transactions are only allowed on the main thread to ensure
        // we're not competing with writes
        releaseAssert(threadIsMain());
    }

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
LedgerTxnRoot::commitChild(EntryIterator iter, RestoredKeys const& restoredKeys,
                           LedgerTxnConsistency cons) noexcept
{
    mImpl->commitChild(std::move(iter), restoredKeys, cons);
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

// Return true only if something is actually accumulated and not skipped over
bool
BulkLedgerEntryChangeAccumulator::accumulate(EntryIterator const& iter)
{
    // Right now, only LEDGER_ENTRY are recorded in the SQL database
    if (iter.key().type() != InternalLedgerEntryType::LEDGER_ENTRY)
    {
        return false;
    }

    // Don't accumulate entry types that are supported by BucketListDB
    auto type = iter.key().ledgerKey().type();
    if (!LiveBucketIndex::typeNotSupported(type))
    {
        return false;
    }

    releaseAssertOrThrow(type == OFFER);
    accum(iter, mOffersToUpsert, mOffersToDelete);
    return true;
}

void
LedgerTxnRoot::Impl::bulkApply(BulkLedgerEntryChangeAccumulator& bleca,
                               size_t bufferThreshold,
                               LedgerTxnConsistency cons)
{

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
}

void
LedgerTxnRoot::Impl::commitChild(EntryIterator iter,
                                 RestoredKeys const& restoredHotArchiveKeys,
                                 LedgerTxnConsistency cons) noexcept
{
    ZoneScoped;

    // In this mode, where we do not start a SQL transaction, so we crash if
    // there's an attempt to commit, since the expected behavior is load and
    // rollback.
    if (!mTransaction)
    {
        printErrorAndAbort("Illegal action in LedgerTxnRoot: committing "
                           "child in read-only mode");
    }

    // Assignment of xdrpp objects does not have the strong exception safety
    // guarantee, so use std::unique_ptr<...>::swap to achieve it
    auto childHeader = std::make_unique<LedgerHeader>(mChild->getHeader());

    auto bleca = BulkLedgerEntryChangeAccumulator();
    [[maybe_unused]] int64_t counter{0};
    try
    {
        while ((bool)iter)
        {
            if (bleca.accumulate(iter))
            {
                ++counter;
            }

            ++iter;
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
        mApp.getDatabase().clearPreparedStatementCache(getSession());
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
    mSession.reset();

    // std::unique_ptr<...>::swap does not throw
    mHeader.swap(childHeader);
    mChild = nullptr;

    mPrefetchHits = 0;
    mPrefetchMisses = 0;

    // std::shared_ptr<...>::reset does not throw
    mSearchableBucketListSnapshot.reset();
}

uint64_t
LedgerTxnRoot::countOffers(LedgerRange const& ledgers) const
{
    return mImpl->countOffers(ledgers);
}

uint64_t
LedgerTxnRoot::Impl::countOffers(LedgerRange const& ledgers) const
{
    using namespace soci;
    throwIfChild();

    std::string query = "SELECT COUNT(*) FROM offers"
                        " WHERE lastmodified >= :v1 AND lastmodified < :v2;";
    uint64_t count = 0;
    int first = static_cast<int>(ledgers.mFirst);
    int limit = static_cast<int>(ledgers.limit());
    getSession().session() << query, into(count), use(first), use(limit);
    return count;
}

void
LedgerTxnRoot::deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const
{
    return mImpl->deleteOffersModifiedOnOrAfterLedger(ledger);
}

void
LedgerTxnRoot::Impl::deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const
{
    using namespace soci;
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    std::string query = "DELETE FROM offers WHERE lastmodified >= :v1";
    getSession().session() << query, use(ledger);
}

void
LedgerTxnRoot::dropOffers()
{
    mImpl->dropOffers();
}

uint32_t
LedgerTxnRoot::prefetchClassic(UnorderedSet<LedgerKey> const& keys)
{
    return mImpl->prefetchClassic(keys);
}

uint32_t
LedgerTxnRoot::prefetchSoroban(UnorderedSet<LedgerKey> const& keys,
                               LedgerKeyMeter* lkMeter)

{
    releaseAssert(lkMeter);
    return mImpl->prefetchSoroban(keys, lkMeter);
}

uint32_t
LedgerTxnRoot::Impl::prefetchSoroban(UnorderedSet<LedgerKey> const& keys,
                                     LedgerKeyMeter* lkMeter)
{
    ZoneScoped;
    return prefetchInternal(keys, lkMeter);
}
uint32_t
LedgerTxnRoot::Impl::prefetchClassic(UnorderedSet<LedgerKey> const& keys)
{
    ZoneScoped;
    return prefetchInternal(keys);
}
uint32_t
LedgerTxnRoot::Impl::prefetchInternal(UnorderedSet<LedgerKey> const& keys,
                                      LedgerKeyMeter* lkMeter)
{
#ifdef BUILD_TESTS
    if (mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        return 0;
    }
#endif

    ZoneScoped;
    uint32_t total = 0;

    auto cacheResult =
        [&](UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>> const&
                res) {
            for (auto const& item : res)
            {
                putInEntryCache(item.first, item.second, LoadType::PREFETCH);
                ++total;
            }
        };

    auto insertIfNotLoaded = [&](auto& keys, LedgerKey const& key) {
        if (!mEntryCache.exists(key, false))
        {
            keys.insert(key);
        }
        else if (lkMeter)
        {
            auto const& mEntry = mEntryCache.get(key);
            if (mEntry.entry)
            {
                // If the key is already in the cache, it still contributes to
                // metering.
                lkMeter->updateReadQuotasForKey(key,
                                                xdr::xdr_size(*mEntry.entry));
            }
        }
    };

    LedgerKeySet keysToSearch;
    for (auto const& key : keys)
    {
        insertIfNotLoaded(keysToSearch, key);
    }
    auto blLoad = getSearchableLiveBucketListSnapshot().loadKeysWithLimits(
        keysToSearch, lkMeter);
    cacheResult(populateLoadedEntries(keysToSearch, blLoad, lkMeter));

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

void
LedgerTxnRoot::prepareNewObjects(size_t s)
{
    mImpl->prepareNewObjects(s);
}

void LedgerTxnRoot::Impl::prepareNewObjects(size_t)
{
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
    prefetchClassic(toPrefetch);
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

SearchableLiveBucketListSnapshot const&
LedgerTxnRoot::Impl::getSearchableLiveBucketListSnapshot() const
{
    if (!mSearchableBucketListSnapshot)
    {
        mSearchableBucketListSnapshot =
            mApp.getBucketManager()
                .getBucketSnapshotManager()
                .copySearchableLiveBucketListSnapshot();
    }

    return *mSearchableBucketListSnapshot;
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
        releaseAssert(!worseThan || isBetterOffer(*worseThan, *iter));

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

        auto le = std::make_shared<LedgerEntry const>(*iter);
        putInEntryCache(LedgerEntryKey(*iter), le, LoadType::IMMEDIATE);

        return le;
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

    UnorderedSet<LedgerKey> toPrefetch;
    UnorderedMap<LedgerKey, LedgerEntry> res(offers.size());
    for (auto const& offer : offers)
    {
        auto key = LedgerEntryKey(offer);
        res.emplace(key, offer);

        auto le = std::make_shared<LedgerEntry const>(offer);
        putInEntryCache(key, le, LoadType::IMMEDIATE);

        auto const& oe = offer.data.offer();
        if (oe.buying.type() != ASSET_TYPE_NATIVE)
        {
            toPrefetch.emplace(trustlineKey(oe.sellerID, oe.buying));
        }
        if (oe.selling.type() != ASSET_TYPE_NATIVE)
        {
            toPrefetch.emplace(trustlineKey(oe.sellerID, oe.selling));
        }
    }
    prefetchClassic(toPrefetch);
    return res;
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxnRoot::getPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                                       Asset const& asset)
{
    return mImpl->getPoolShareTrustLinesByAccountAndAsset(account, asset);
}

UnorderedMap<LedgerKey, LedgerEntry>
LedgerTxnRoot::Impl::getPoolShareTrustLinesByAccountAndAsset(
    AccountID const& account, Asset const& asset)
{
    ZoneScoped;
    std::vector<LedgerEntry> trustLines;
    try
    {
        trustLines =
            getSearchableLiveBucketListSnapshot()
                .loadPoolShareTrustLinesByAccountAndAsset(account, asset);
    }
    catch (NonSociRelatedException&)
    {
        throw;
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error when getting pool share trust lines by "
                           "account and asset from LedgerTxnRoot: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("unknown fatal error when getting offers by account "
                           "and asset from LedgerTxnRoot");
    }

    UnorderedMap<LedgerKey, LedgerEntry> res(trustLines.size());
    for (auto const& tl : trustLines)
    {
        auto key = LedgerEntryKey(tl);
        res.emplace(key, tl);

        auto le = std::make_shared<LedgerEntry const>(tl);
        putInEntryCache(key, le, LoadType::IMMEDIATE);
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
        return getSearchableLiveBucketListSnapshot().loadInflationWinners(
            maxWinners, minVotes);
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
        if (key.type() != OFFER)
        {
            entry = getSearchableLiveBucketListSnapshot().load(key);
        }
        else
        {
            entry = loadOffer(key);
        }
    }
    catch (NonSociRelatedException&)
    {
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
LedgerTxnRoot::rollbackChild() noexcept
{
    mImpl->rollbackChild();
}

void
LedgerTxnRoot::Impl::rollbackChild() noexcept
{
    if (mTransaction)
    {
        try
        {
            mTransaction->rollback();
            mTransaction.reset();
            mSession.reset();
        }
        catch (std::exception& e)
        {
            printErrorAndAbort(
                "fatal error when rolling back child of LedgerTxnRoot: ",
                e.what());
        }
        catch (...)
        {
            printErrorAndAbort(
                "unknown fatal error when rolling back child of LedgerTxnRoot");
        }
    }

    mChild = nullptr;
    mPrefetchHits = 0;
    mPrefetchMisses = 0;
    mSearchableBucketListSnapshot.reset();
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
