// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledgerdelta/LedgerDeltaLayer.h"
#include "ledger/AccountFrame.h"
#include "ledger/TrustFrame.h"
// #include "util/types.h"
// #include "xdr/Stellar-ledger.h"
// #include "xdrpp/printer.h"

namespace stellar
{
using xdr::operator==;

LedgerDeltaLayer::LedgerDeltaLayer(LedgerHeader header)
    : mHeader{std::move(header)}
{
}

LedgerHeader&
LedgerDeltaLayer::getHeader()
{
    return mHeader.mHeader;
}

LedgerHeader const&
LedgerDeltaLayer::getHeader() const
{
    return mHeader.mHeader;
}

LedgerHeaderFrame&
LedgerDeltaLayer::getHeaderFrame()
{
    return mHeader;
}

bool
LedgerDeltaLayer::isValid(LedgerEntry const& entry) const
{
    switch (entry.data.type())
    {
    case ACCOUNT:
        return AccountFrame{entry}.isValid();
    case DATA:
        return true;
    case TRUSTLINE:
        return TrustFrame{entry}.isValid();
    case OFFER:
        return true;
    }
}

bool
LedgerDeltaLayer::shouldStore(LedgerEntry const& entry) const
{
    if (entry.data.type() != TRUSTLINE)
    {
        return true;
    }

    return !TrustFrame{entry}.isIssuer();
}

bool
LedgerDeltaLayer::addEntry(EntryFrame& entry)
{
    assert(isValid(entry.getEntry()));
    if (!shouldStore(entry.getEntry()))
    {
        return false;
    }

    touch(entry);

    auto k = entry.getKey();
    auto del_it = mDelete.find(k);
    if (del_it != mDelete.end())
    {
        // delete + new is an update
        mDelete.erase(del_it);
        mMod[k] = entry.getEntry();
    }
    else
    {
        if (mNew.find(k) != mNew.end()     // double new
            || mMod.find(k) != mMod.end()) // mod + new is invalid
        {
            throw std::runtime_error("Could not update data in LedgerDeltaLayer");
        }
        mNew[k] = entry.getEntry();
    }

    return true;
}

void
LedgerDeltaLayer::deleteEntry(LedgerKey const& key)
{
    auto new_it = mNew.find(key);
    if (new_it != mNew.end())
    {
        // new + delete -> don't add it in the first place
        mNew.erase(new_it);
    }
    else
    {
        if (mDelete.find(key) == mDelete.end())
        {
            mDelete.insert(key);
        } // else already being deleted

        mMod.erase(key);
    }
}

bool
LedgerDeltaLayer::updateEntry(EntryFrame& entry)
{
    assert(isValid(entry.getEntry()));
    if (!shouldStore(entry.getEntry()))
    {
        return false;
    }

    touch(entry);

    auto k = entry.getKey();
    auto mod_it = mMod.find(k);
    if (mod_it != mMod.end())
    {
        // collapse mod
        mod_it->second = entry.getEntry();
    }
    else
    {
        auto new_it = mNew.find(k);
        if (new_it != mNew.end())
        {
            // new + mod = new (with latest value)
            new_it->second = entry.getEntry();
        }
        else
        {
            if (mDelete.find(k) != mDelete.end()) // delete + mod is illegal
            {
                throw std::runtime_error(
                    "Could not update data in LedgerDeltaLayer");
            }
            mMod[k] = entry.getEntry();
        }
    }

    return true;
}

void
LedgerDeltaLayer::recordEntry(EntryFrame const& entry)
{
    auto k = entry.getKey();

    // keeps the old one around
    mPrevious.insert(std::make_pair(k, entry.getEntry()));
}

void
LedgerDeltaLayer::mergeEntries(LedgerDeltaLayer const& other)
{
    // propagates mPrevious for deleted & modified entries
    for (auto& d : other.mDelete)
    {
        deleteEntry(d);
        auto it = other.mPrevious.find(d);
        if (it != other.mPrevious.end())
        {
            recordEntry(EntryFrame{it->second});
        }
    }
    for (auto& n : other.mNew)
    {
        EntryFrame frame{n.second};
        addEntry(frame);
    }
    for (auto& m : other.mMod)
    {
        EntryFrame frame{m.second};
        updateEntry(frame);
        auto it = other.mPrevious.find(m.first);
        if (it != other.mPrevious.end())
        {
            recordEntry(EntryFrame{it->second});
        }
    }
}

void
LedgerDeltaLayer::apply(LedgerDeltaLayer const& delta)
{
    mergeEntries(delta);
    mHeader = delta.mHeader;
}

void
LedgerDeltaLayer::addCurrentMeta(LedgerEntryChanges& changes,
                            LedgerKey const& key) const
{
    auto it = mPrevious.find(key);
    if (it != mPrevious.end())
    {
        // if the old value is from a previous ledger we emit it
        auto const& e = it->second;
        if (e.lastModifiedLedgerSeq != mHeader.mHeader.ledgerSeq)
        {
            changes.emplace_back(LEDGER_ENTRY_STATE);
            changes.back().state() = e;
        }
    }
}

LedgerEntryChanges
LedgerDeltaLayer::getChanges() const
{
    LedgerEntryChanges changes;

    for (auto const& k : mNew)
    {
        changes.emplace_back(LEDGER_ENTRY_CREATED);
        changes.back().created() = k.second;
    }
    for (auto const& k : mMod)
    {
        addCurrentMeta(changes, k.first);
        changes.emplace_back(LEDGER_ENTRY_UPDATED);
        changes.back().updated() = k.second;
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
LedgerDeltaLayer::getLiveEntries() const
{
    std::vector<LedgerEntry> live;

    live.reserve(mNew.size() + mMod.size());

    for (auto const& k : mNew)
    {
        live.push_back(k.second);
    }
    for (auto const& k : mMod)
    {
        live.push_back(k.second);
    }

    return live;
}

std::vector<LedgerKey>
LedgerDeltaLayer::getDeadEntries() const
{
    std::vector<LedgerKey> dead;

    dead.reserve(mDelete.size());

    for (auto const& k : mDelete)
    {
        dead.push_back(k);
    }
    return dead;
}

void
LedgerDeltaLayer::touch(EntryFrame& entry)
{
    auto ledgerSeq = getHeader().ledgerSeq;
    assert(ledgerSeq != 0);
    entry.getEntry().lastModifiedLedgerSeq = ledgerSeq;
}
}
