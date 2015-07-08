#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <set>
#include "ledger/EntryFrame.h"
#include "ledger/LedgerHeaderFrame.h"
#include "bucket/LedgerCmp.h"
#include "xdrpp/marshal.h"

namespace stellar
{
class Application;

class LedgerDelta
{
    typedef std::map<LedgerKey, EntryFrame::pointer, LedgerEntryIdCmp>
        KeyEntryMap;

    LedgerDelta*
        mOuterDelta;       // set when this delta is nested inside another delta
    LedgerHeader* mHeader; // LedgerHeader to commit changes to

    // objects to keep track of changes
    // ledger header itself
    LedgerHeaderFrame mCurrentHeader;
    LedgerHeader mPreviousHeaderValue;
    // ledger entries
    KeyEntryMap mNew;
    KeyEntryMap mMod;
    std::set<LedgerKey, LedgerEntryIdCmp> mDelete;

    void checkState();
    void addEntry(EntryFrame::pointer entry);
    void deleteEntry(EntryFrame::pointer entry);
    void modEntry(EntryFrame::pointer entry);

    // merge "other" into current ledgerDelta
    void mergeEntries(LedgerDelta& other);

  public:
    // keeps an internal reference to the outerDelta,
    // will apply changes to the outer scope on commit
    explicit LedgerDelta(LedgerDelta& outerDelta);

    // keeps an internal reference to ledgerHeader,
    // will apply changes to ledgerHeader on commit
    LedgerDelta(LedgerHeader& ledgerHeader);

    LedgerHeader& getHeader();
    LedgerHeaderFrame& getHeaderFrame();

    // methods to register changes in the ledger entries
    void addEntry(EntryFrame const& entry);
    void deleteEntry(EntryFrame const& entry);
    void deleteEntry(LedgerKey const& key);
    void modEntry(EntryFrame const& entry);

    // commits this delta into outer delta
    void commit();
    // aborts any changes pending
    void rollback();

    void markMeters(Application& app) const;

    std::vector<LedgerEntry> getLiveEntries() const;
    std::vector<LedgerKey> getDeadEntries() const;

    LedgerEntryChanges getChanges() const;
};
}
