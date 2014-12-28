#ifndef __LEDGERDELTA__
#define __LEDGERDELTA__

#include <map>
#include "ledger/EntryFrame.h"

namespace stellar
{
    class LedgerDelta
    {
        std::map<uint256, EntryFrame::pointer> mNew;
        std::map<uint256, EntryFrame::pointer> mMod;
        std::map<uint256, EntryFrame::pointer> mDelete;
    public:

        void addEntry(EntryFrame& entry);
        void deleteEntry(EntryFrame& entry);
        void modEntry(EntryFrame& entry);

    };
}

#endif
