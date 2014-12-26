#ifndef __LEDGERDELTA__
#define __LEDGERDELTA__

#include <map>
#include "ledger/LedgerEntry.h"

namespace stellar
{
    class LedgerDelta
    {
        std::map<stellarxdr::uint256, LedgerEntry::pointer> mNew;
        std::map<stellarxdr::uint256, LedgerEntry::pointer> mMod;
        std::map<stellarxdr::uint256, LedgerEntry::pointer> mDelete;
    public:

        void addEntry(LedgerEntry::pointer entry);
        void deleteEntry(LedgerEntry::pointer entry);
        void modEntry(LedgerEntry::pointer entry);

    };
}

#endif
