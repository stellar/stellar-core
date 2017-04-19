#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "ledger/LedgerHeaderFrame.h"

#include <map>
#include <set>

namespace stellar
{
class Application;
class EntryFrame;
class LedgerDelta;

class LedgerDeltaLayer final
{
  public:
    using KeyEntryMap = std::map<LedgerKey, LedgerEntry, LedgerEntryIdCmp>;

    explicit LedgerDeltaLayer(LedgerHeader ledgerHeader);

    std::vector<LedgerEntry> getLiveEntries() const;
    std::vector<LedgerKey> getDeadEntries() const;

    LedgerEntryChanges getChanges() const;

    KeyEntryMap const&
    newEntries() const
    {
        return mNew;
    }
    KeyEntryMap const&
    updatedEntries() const
    {
        return mMod;
    }
    std::set<LedgerKey, LedgerEntryIdCmp> const&
    deletedEntries() const
    {
        return mDelete;
    }

private:
    friend LedgerDelta;
    LedgerHeaderFrame mHeader; // LedgerHeader to commit changes to

    // ledger entries
    KeyEntryMap mNew;
    KeyEntryMap mMod;
    std::set<LedgerKey, LedgerEntryIdCmp> mDelete;
    KeyEntryMap mPrevious;

    // merge "other" into current ledgerDelta
    void mergeEntries(LedgerDeltaLayer const& other);

    // helper method that adds a meta entry to "changes"
    // with the previous value of an entry if needed
    void addCurrentMeta(LedgerEntryChanges& changes,
                        LedgerKey const& key) const;

    bool isValid(LedgerEntry const& entry) const;
    bool shouldStore(LedgerEntry const& entry) const;

    bool addEntry(EntryFrame& entry);
    void deleteEntry(LedgerKey const& key);
    bool updateEntry(EntryFrame& entry);
    void recordEntry(EntryFrame const& entry);

    LedgerHeader& getHeader();
    LedgerHeader const& getHeader() const;
    LedgerHeaderFrame& getHeaderFrame();

    // applies other delta into this one
    void apply(LedgerDeltaLayer const& delta);

    // touch the entry if the delta is tracking a ledger header with
    // a sequence that is not 0 (0 is used when importing buckets)
    void touch(EntryFrame& entry);
};
}
