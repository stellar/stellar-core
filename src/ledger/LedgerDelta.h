#pragma once

#include <map>
#include <set>
#include "ledger/EntryFrame.h"
#include "clf/LedgerCmp.h"

namespace stellar
{
    class LedgerDelta
    {
        std::map<LedgerKey, EntryFrame::pointer, LedgerEntryIdCmp> mNew;
        std::map<LedgerKey, EntryFrame::pointer, LedgerEntryIdCmp> mMod;
        std::set<LedgerKey, LedgerEntryIdCmp> mDelete;

        void addEntry(EntryFrame::pointer entry);
        void deleteEntry(EntryFrame::pointer entry);
        void modEntry(EntryFrame::pointer entry);

    public:

        void addEntry(EntryFrame const& entry);
        void deleteEntry(EntryFrame const& entry);
        void deleteEntry(LedgerKey const& key);
        void modEntry(EntryFrame const& entry);

        // apply other on top of delta, collapsing entries as appropriate
        void merge(LedgerDelta &other);
    };
}


