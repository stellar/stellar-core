// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/printer.h"

namespace stellar
{
using xdr::operator==;

LedgerDelta::LedgerDelta(LedgerDelta& outerDelta)
    : mOuterDelta(&outerDelta)
    , mHeader(&outerDelta.getHeader())
    , mCurrentHeader(outerDelta.getHeader())
    , mPreviousHeaderValue(outerDelta.getHeader())
    , mDb(outerDelta.mDb)
    , mUpdateLastModified(outerDelta.mUpdateLastModified)
{
}

LedgerDelta::LedgerDelta(LedgerHeader& header, Database& db,
                         bool updateLastModified)
    : mOuterDelta(nullptr)
    , mHeader(&header)
    , mCurrentHeader(header)
    , mPreviousHeaderValue(header)
    , mDb(db)
    , mUpdateLastModified(updateLastModified)
{
}

LedgerDelta::~LedgerDelta()
{
    if (mHeader)
    {
        rollback();
    }
}

LedgerHeader&
LedgerDelta::getHeader()
{
    return mCurrentHeader.mHeader;
}

LedgerHeader const&
LedgerDelta::getHeader() const
{
    return mCurrentHeader.mHeader;
}

LedgerHeaderFrame&
LedgerDelta::getHeaderFrame()
{
    return mCurrentHeader;
}

LedgerHeader const&
LedgerDelta::getPreviousHeader() const
{
    return mPreviousHeaderValue;
}

void
LedgerDelta::checkState()
{
    if (mHeader == nullptr)
    {
        throw std::runtime_error(
            "Invalid operation: delta is already committed");
    }
}

void
LedgerDelta::addEntry(EntryFrame const& entry)
{
    addEntry(entry.copy());
}

void
LedgerDelta::deleteEntry(EntryFrame const& entry)
{
    deleteEntry(entry.copy());
}

void
LedgerDelta::modEntry(EntryFrame const& entry)
{
    modEntry(entry.copy());
}

void
LedgerDelta::recordEntry(EntryFrame const& entry)
{
    recordEntry(entry.copy());
}

void
LedgerDelta::addEntry(EntryFrame::pointer entry)
{
    checkState();
    auto k = entry->getKey();
    auto del_it = mDelete.find(k);
    if (del_it != mDelete.end())
    {
        // delete + new is an update
        mDelete.erase(del_it);
        mMod[k] = entry;
    }
    else
    {
        assert(mNew.find(k) == mNew.end()); // double new
        assert(mMod.find(k) == mMod.end()); // mod + new is invalid
        mNew[k] = entry;
    }
}

void
LedgerDelta::deleteEntry(EntryFrame::pointer entry)
{
    auto k = entry->getKey();
    deleteEntry(k);
}

void
LedgerDelta::deleteEntry(LedgerKey const& k)
{
    checkState();
    auto new_it = mNew.find(k);
    if (new_it != mNew.end())
    {
        // new + delete -> don't add it in the first place
        mNew.erase(new_it);
    }
    else
    {
        // double delete here means there is buggy code upstream
        // and we cannot keep going as this may corrupt the bucket list
        assert(mDelete.find(k) == mDelete.end());

        // mod + delete -> delete
        mMod.erase(k);
        mDelete.insert(k);
    }
}

void
LedgerDelta::modEntry(EntryFrame::pointer entry)
{
    checkState();
    auto k = entry->getKey();
    auto mod_it = mMod.find(k);
    if (mod_it != mMod.end())
    {
        // collapse mod
        mod_it->second = entry;
    }
    else
    {
        auto new_it = mNew.find(k);
        if (new_it != mNew.end())
        {
            // new + mod = new (with latest value)
            new_it->second = entry;
        }
        else
        {
            assert(mDelete.find(k) == mDelete.end()); // delete + mod is illegal
            mMod[k] = entry;
        }
    }
}

void
LedgerDelta::recordEntry(EntryFrame::pointer entry)
{
    checkState();
    // keeps the old one around
    mPrevious.insert(std::make_pair(entry->getKey(), entry));
}

void
LedgerDelta::mergeEntries(LedgerDelta& other)
{
    checkState();

    // propagates mPrevious for deleted & modified entries
    for (auto& d : other.mDelete)
    {
        deleteEntry(d);
        auto it = other.mPrevious.find(d);
        if (it != other.mPrevious.end())
        {
            recordEntry(*it->second);
        }
    }
    for (auto& n : other.mNew)
    {
        addEntry(n.second);
    }
    for (auto& m : other.mMod)
    {
        modEntry(m.second);
        auto it = other.mPrevious.find(m.first);
        if (it != other.mPrevious.end())
        {
            recordEntry(*it->second);
        }
    }
}

void
LedgerDelta::commit()
{
    checkState();
    // checks if we about to override changes that were made
    // outside of this LedgerDelta
    if (!(mPreviousHeaderValue == *mHeader))
    {
        throw std::runtime_error("unexpected header state");
    }

    if (mOuterDelta)
    {
        mOuterDelta->mergeEntries(*this);
        mOuterDelta = nullptr;
    }
    *mHeader = mCurrentHeader.mHeader;
    mHeader = nullptr;
}

void
LedgerDelta::rollback()
{
    checkState();
    mHeader = nullptr;

    for (auto& d : mDelete)
    {
        EntryFrame::flushCachedEntry(d, mDb);
    }
    for (auto& n : mNew)
    {
        EntryFrame::flushCachedEntry(n.first, mDb);
    }
    for (auto& m : mMod)
    {
        EntryFrame::flushCachedEntry(m.first, mDb);
    }
}

void
LedgerDelta::addCurrentMeta(LedgerEntryChanges& changes,
                            LedgerKey const& key) const
{
    auto it = mPrevious.find(key);
    if (it != mPrevious.end())
    {
        auto const& e = it->second->mEntry;
        changes.emplace_back(LEDGER_ENTRY_STATE);
        changes.back().state() = e;
    }
}

LedgerEntryChanges
LedgerDelta::getChanges() const
{
    LedgerEntryChanges changes;

    for (auto const& k : mNew)
    {
        changes.emplace_back(LEDGER_ENTRY_CREATED);
        changes.back().created() = k.second->mEntry;
    }
    for (auto const& k : mMod)
    {
        addCurrentMeta(changes, k.first);
        changes.emplace_back(LEDGER_ENTRY_UPDATED);
        changes.back().updated() = k.second->mEntry;
    }

    for (auto const& k : mDelete)
    {
        addCurrentMeta(changes, k);
        changes.emplace_back(LEDGER_ENTRY_REMOVED);
        changes.back().removed() = k;
    }

    return changes;
}

std::vector<LedgerEntry>
LedgerDelta::getLiveEntries() const
{
    std::vector<LedgerEntry> live;

    live.reserve(mNew.size() + mMod.size());

    for (auto const& k : mNew)
    {
        live.push_back(k.second->mEntry);
    }
    for (auto const& k : mMod)
    {
        live.push_back(k.second->mEntry);
    }

    return live;
}

std::vector<LedgerKey>
LedgerDelta::getDeadEntries() const
{
    std::vector<LedgerKey> dead;

    dead.reserve(mDelete.size());

    for (auto const& k : mDelete)
    {
        dead.push_back(k);
    }
    return dead;
}

bool
LedgerDelta::updateLastModified() const
{
    return mUpdateLastModified;
}

void
LedgerDelta::markMeters(Application& app) const
{
    for (auto const& ke : mNew)
    {
        switch (ke.first.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "add"}, "entry")
                .Mark();
            break;
        case TRUSTLINE:
            app.getMetrics()
                .NewMeter({"ledger", "trust", "add"}, "entry")
                .Mark();
            break;
        case OFFER:
            app.getMetrics()
                .NewMeter({"ledger", "offer", "add"}, "entry")
                .Mark();
            break;
        case DATA:
            app.getMetrics()
                .NewMeter({"ledger", "data", "add"}, "entry")
                .Mark();
            break;
        }
    }

    for (auto const& ke : mMod)
    {
        switch (ke.first.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "modify"}, "entry")
                .Mark();
            break;
        case TRUSTLINE:
            app.getMetrics()
                .NewMeter({"ledger", "trust", "modify"}, "entry")
                .Mark();
            break;
        case OFFER:
            app.getMetrics()
                .NewMeter({"ledger", "offer", "modify"}, "entry")
                .Mark();
            break;
        case DATA:
            app.getMetrics()
                .NewMeter({"ledger", "data", "modify"}, "entry")
                .Mark();
            break;
        }
    }

    for (auto const& ke : mDelete)
    {
        switch (ke.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "delete"}, "entry")
                .Mark();
            break;
        case TRUSTLINE:
            app.getMetrics()
                .NewMeter({"ledger", "trust", "delete"}, "entry")
                .Mark();
            break;
        case OFFER:
            app.getMetrics()
                .NewMeter({"ledger", "offer", "delete"}, "entry")
                .Mark();
            break;
        case DATA:
            app.getMetrics()
                .NewMeter({"ledger", "data", "delete"}, "entry")
                .Mark();
            break;
        }
    }
}

template <typename IterType, typename ValueType>
void
LedgerDelta::Iterator<IterType, ValueType>::createValueIfNecessary() const
{
    if (!mValue)
    {
        mValue = std::make_shared<ValueType>(mDelta, *mIter);
    }
}

template <typename IterType, typename ValueType>
LedgerDelta::Iterator<IterType, ValueType>::Iterator(LedgerDelta const& delta,
                                                     IterType const& iter)
    : mDelta(delta), mIter(iter)
{
}

template <typename IterType, typename ValueType>
ValueType const& LedgerDelta::Iterator<IterType, ValueType>::operator*() const
{
    createValueIfNecessary();
    return *mValue;
}

template <typename IterType, typename ValueType>
ValueType const* LedgerDelta::Iterator<IterType, ValueType>::operator->() const
{
    createValueIfNecessary();
    return mValue.get();
}

template <typename IterType, typename ValueType>
LedgerDelta::Iterator<IterType, ValueType>&
    LedgerDelta::Iterator<IterType, ValueType>::operator++()
{
    ++mIter;
    mValue.reset();
    return *this;
}

template <typename IterType, typename ValueType>
bool
LedgerDelta::Iterator<IterType, ValueType>::
operator==(Iterator<IterType, ValueType> const& other) const
{
    return mIter == other.mIter;
}

template <typename IterType, typename ValueType>
bool
LedgerDelta::Iterator<IterType, ValueType>::
operator!=(Iterator<IterType, ValueType> const& other) const
{
    return !(mIter == other.mIter);
}

template <typename IterType>
LedgerDelta::IteratorRange<IterType>::IteratorRange(IterType const& begin,
                                                    IterType const& end)
    : mBegin(begin), mEnd(end)
{
}

template <typename IterType>
IterType
LedgerDelta::IteratorRange<IterType>::begin() const
{
    return mBegin;
}

template <typename IterType>
IterType
LedgerDelta::IteratorRange<IterType>::end() const
{
    return mEnd;
}

template class LedgerDelta::Iterator<LedgerDelta::KeyEntryMap::const_iterator,
                                     LedgerDelta::AddedLedgerEntry>;
template class LedgerDelta::IteratorRange<LedgerDelta::AddedIterator>;

LedgerDelta::AddedLedgerEntry::AddedLedgerEntry(
    LedgerDelta const& delta, KeyEntryMap::value_type const& pair)
    : key(pair.first), current(pair.second)
{
}

LedgerDelta::IteratorRange<LedgerDelta::AddedIterator>
LedgerDelta::added() const
{
    return {LedgerDelta::AddedIterator(*this, mNew.cbegin()),
            LedgerDelta::AddedIterator(*this, mNew.cend())};
}

template class LedgerDelta::Iterator<LedgerDelta::KeyEntryMap::const_iterator,
                                     LedgerDelta::ModifiedLedgerEntry>;
template class LedgerDelta::IteratorRange<LedgerDelta::ModifiedIterator>;

LedgerDelta::ModifiedLedgerEntry::ModifiedLedgerEntry(
    LedgerDelta const& delta, KeyEntryMap::value_type const& pair)
    : key(pair.first)
    , current(pair.second)
    , previous(delta.mPrevious.at(pair.first))
{
}

LedgerDelta::IteratorRange<LedgerDelta::ModifiedIterator>
LedgerDelta::modified() const
{
    return {LedgerDelta::ModifiedIterator(*this, mMod.cbegin()),
            LedgerDelta::ModifiedIterator(*this, mMod.cend())};
}

template class LedgerDelta::Iterator<
    std::set<LedgerKey, LedgerEntryIdCmp>::const_iterator,
    LedgerDelta::DeletedLedgerEntry>;
template class LedgerDelta::IteratorRange<LedgerDelta::DeletedIterator>;

LedgerDelta::DeletedLedgerEntry::DeletedLedgerEntry(LedgerDelta const& delta,
                                                    LedgerKey const& value)
    : key(value), previous(delta.mPrevious.at(value))
{
}

LedgerDelta::IteratorRange<LedgerDelta::DeletedIterator>
LedgerDelta::deleted() const
{
    return {LedgerDelta::DeletedIterator(*this, mDelete.cbegin()),
            LedgerDelta::DeletedIterator(*this, mDelete.cend())};
}
}
