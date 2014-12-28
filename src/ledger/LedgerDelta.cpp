#include "ledger/LedgerDelta.h"

namespace stellar
{
    void LedgerDelta::addEntry(EntryFrame& entry)
    {
        mNew[entry.getIndex()] = entry.copy();
    }

    void LedgerDelta::deleteEntry(EntryFrame& entry)
    {
        mMod.erase(entry.getIndex());
        mNew.erase(entry.getIndex());
        mDelete[entry.getIndex()] = entry.copy();
    }

    void LedgerDelta::modEntry(EntryFrame& entry)
    {
        // check if it was just made
        if(mNew.find(entry.getIndex()) != mNew.end())
        {
            mNew[entry.getIndex()] = entry.copy();
        } else
        {
            mMod[entry.getIndex()] = entry.copy();
        }
    }

    //////////////////////////////////////////////////////////////////////////

    
}
