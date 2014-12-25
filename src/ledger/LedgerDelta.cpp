#include "ledger/LedgerDelta.h"

namespace stellar
{
    void LedgerDelta::addEntry(LedgerEntry::pointer entry)
    {
        mNew[entry->getIndex()] = entry;
    }

    void LedgerDelta::deleteEntry(LedgerEntry::pointer entry)
    {
        mMod.erase(entry->getIndex());
        mNew.erase(entry->getIndex());
        mDelete[entry->getIndex()] = entry;
    }

    void LedgerDelta::modEntry(LedgerEntry::pointer entry)
    {
        // check if it was just made
        if(mNew.find(entry->getIndex()) != mNew.end())
        {
            mNew[entry->getIndex()] = entry;
        } else
        {
            mMod[entry->getIndex()] = entry;
        }
    }

    //////////////////////////////////////////////////////////////////////////

    
}
