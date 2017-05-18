// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledgerdelta/LedgerDelta.h"
#include "database/EntryQueries.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerEntries.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "ledgerdelta/LedgerDeltaLayer.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/types.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

#include <cassert>

namespace stellar
{

using xdr::operator==;

LedgerDelta::LedgerDelta(LedgerHeader ledgerHeader, LedgerEntries& entries)
    : mEntries{entries}
{
    mLayers.emplace_back(ledgerHeader);
}

LedgerDelta::~LedgerDelta() = default;

bool
LedgerDelta::isCollapsed() const
{
    return mLayers.size() == 1;
}

void
LedgerDelta::newDelta()
{
    assert(!mLayers.empty());
    mLayers.emplace_back(top().getHeader());
}

void
LedgerDelta::applyTop()
{
    assert(!mLayers.empty());
    auto previous = mLayers.back();
    mLayers.pop_back();

    assert(!mLayers.empty());
    top().apply(previous);
}

void
LedgerDelta::rollbackTop()
{
    assert(!mLayers.empty());
    mLayers.pop_back();
}

LedgerDeltaLayer const&
LedgerDelta::top() const
{
    assert(!mLayers.empty());
    return mLayers.back();
}

LedgerDeltaLayer&
LedgerDelta::top()
{
    assert(!mLayers.empty());
    return mLayers.back();
}

LedgerHeader&
LedgerDelta::getHeader()
{
    return top().getHeader();
}

LedgerHeader const&
LedgerDelta::getHeader() const
{
    return top().getHeader();
}

LedgerHeaderFrame&
LedgerDelta::getHeaderFrame()
{
    return top().getHeaderFrame();
}

void
LedgerDelta::insertOrUpdateEntry(EntryFrame& entry)
{
    if (entryExists(entry.getKey()))
    {
        updateEntry(entry);
    }
    else
    {
        addEntry(entry);
    }
}

void
LedgerDelta::addEntry(EntryFrame& entry)
{
    if (top().addEntry(entry))
    {
        stellar::insertEntry(entry.getEntry(), mEntries.getDatabase());
    }
}

void
LedgerDelta::deleteEntry(LedgerKey const& key)
{
    top().deleteEntry(key);
    stellar::deleteEntry(key, mEntries.getDatabase());
}

void
LedgerDelta::updateEntry(EntryFrame& entry)
{
    if (top().updateEntry(entry))
    {
        stellar::updateEntry(entry.getEntry(), mEntries.getDatabase());
    }
}

void
LedgerDelta::recordEntry(EntryFrame const& entry)
{
    top().recordEntry(entry);
}

bool
LedgerDelta::entryExists(LedgerKey const& key) const
{
    for (auto i = mLayers.rbegin(); i != mLayers.rend(); ++i)
    {
        if (i->deletedEntries().find(key) != std::end(i->deletedEntries()))
        {
            return false;
        }

        auto newIt = i->newEntries().find(key);
        if (newIt != std::end(i->newEntries()))
        {
            return true;
        }

        auto modIt = i->updatedEntries().find(key);
        if (modIt != std::end(i->updatedEntries()))
        {
            return true;
        }
    }

    return mEntries.exists(key);
}

LedgerEntryChanges
LedgerDelta::getChanges() const
{
    return top().getChanges();
}

optional<LedgerEntry const>
LedgerDelta::loadEntry(LedgerKey const& key)
{
    for (auto i = mLayers.rbegin(); i != mLayers.rend(); ++i)
    {
        if (i->deletedEntries().find(key) != std::end(i->deletedEntries()))
        {
            return nullopt<LedgerEntry const>();
        }

        auto newIt = i->newEntries().find(key);
        if (newIt != std::end(i->newEntries()))
        {
            return make_optional<LedgerEntry const>(newIt->second);
        }

        auto modIt = i->updatedEntries().find(key);
        if (modIt != std::end(i->updatedEntries()))
        {
            return make_optional<LedgerEntry const>(modIt->second);
        }
    }

    auto e = mEntries.load(key);
    if (e)
    {
        top().recordEntry(EntryFrame{*e});
    }
    return e;
}

optional<LedgerEntry const>
LedgerDelta::loadAccount(AccountID accountID)
{
    return loadEntry(accountKey(std::move(accountID)));
}

optional<LedgerEntry const>
LedgerDelta::loadData(AccountID accountID, std::string name)
{
    return loadEntry(dataKey(std::move(accountID), std::move(name)));
}

optional<LedgerEntry const>
LedgerDelta::loadTrustLine(AccountID accountID, Asset asset)
{
    if (accountID == getIssuer(asset))
    {
        return make_optional<LedgerEntry const>(TrustFrame{asset}.getEntry());
    }
    return loadEntry(trustLineKey(std::move(accountID), std::move(asset)));
}

optional<LedgerEntry const>
LedgerDelta::loadOffer(AccountID sellerID, uint64_t offerID)
{
    return loadEntry(offerKey(std::move(sellerID), offerID));
}

void
LedgerDelta::markMeters(Application& app) const
{
    for (auto const& ke : top().newEntries())
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

    for (auto const& ke : top().updatedEntries())
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

    for (auto const& ke : top().deletedEntries())
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
}
