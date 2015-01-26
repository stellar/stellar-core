#pragma once

#include <map>
#include "ledger/EntryFrame.h"

namespace stellar
{
    class LedgerDelta
    {
        std::map<uint256, EntryFrame::pointer> mNew;
        std::map<uint256, EntryFrame::pointer> mMod;
        std::map<uint256, EntryFrame::pointer> mDelete;

        void addEntry(EntryFrame::pointer entry);
        void deleteEntry(EntryFrame::pointer entry);
        void modEntry(EntryFrame::pointer entry);

    public:

        void addEntry(EntryFrame const& entry);
        void deleteEntry(EntryFrame const& entry);
        void modEntry(EntryFrame const& entry);

        // apply other on top of delta, collapsing entries as appropriate
        void merge(LedgerDelta &other);
    };
}


