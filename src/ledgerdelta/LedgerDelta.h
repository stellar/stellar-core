#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHeaderFrame.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-ledger-entries.h"
#include "util/optional.h"

#include <vector>

namespace stellar
{

class Application;
class EntryFrame;
class LedgerDeltaLayer;
class LedgerEntries;

class LedgerDelta
{
public:
    explicit LedgerDelta(LedgerHeader ledgerHeader, LedgerEntries& entries);
    ~LedgerDelta();

    bool isCollapsed() const;

    LedgerDeltaLayer const& top() const;
    LedgerDeltaLayer &top();

    LedgerHeader& getHeader();
    LedgerHeader const& getHeader() const;
    LedgerHeaderFrame& getHeaderFrame();

    void insertOrUpdateEntry(EntryFrame& entry);
    void addEntry(EntryFrame& entry);
    void deleteEntry(LedgerKey const& key);
    void updateEntry(EntryFrame& entry);
    void recordEntry(EntryFrame const& entry);
    bool entryExists(LedgerKey const& key) const;

    LedgerEntryChanges getChanges() const;

    optional<LedgerEntry const> loadAccount(AccountID accountID);
    optional<LedgerEntry const> loadData(AccountID accountID, std::string name);
    optional<LedgerEntry const> loadTrustLine(AccountID accountID, Asset asset);
    optional<LedgerEntry const> loadOffer(AccountID sellerID, uint64_t offerID);

    void markMeters(Application& app) const;

private:
    LedgerEntries& mEntries;
    std::vector<LedgerDeltaLayer> mLayers;

    optional<LedgerEntry const> loadEntry(LedgerKey const& key);

    friend class LedgerDeltaScope;
    void newDelta();
    void applyTop();
    void rollbackTop();
};

}
