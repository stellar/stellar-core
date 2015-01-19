#include "ledger/LedgerDelta.h"

namespace stellar
{
    void LedgerDelta::addEntry(EntryFrame& entry)
    {
        addEntry(entry.copy());
    }

    void LedgerDelta::deleteEntry(EntryFrame& entry)
    {
        deleteEntry(entry.copy());
    }

    void LedgerDelta::modEntry(EntryFrame& entry)
    {
        modEntry(entry.copy());
    }

    void LedgerDelta::addEntry(EntryFrame::pointer& entry)
    {
        Hash index = entry->getIndex();
        auto del_it = mDelete.find(index);
        if (del_it != mDelete.end())
        {
            // delete + new is an update
            mDelete.erase(del_it);
            mMod[index] = entry;
        }
        else
        {
            assert(mNew.find(index) == mNew.end()); // double new
            assert(mMod.find(index) == mMod.end()); // mod + new is invalid
            mNew[index] = entry;
        }
    }

    void LedgerDelta::deleteEntry(EntryFrame::pointer& entry)
    {
        Hash index = entry->getIndex();
        auto new_it = mNew.find(index);
        if (new_it != mNew.end())
        {
            // new + delete -> don't add it in the first place
            mNew.erase(new_it);
        }
        else
        {
            assert(mDelete.find(index) == mDelete.end()); // double delete is invalid
            // only keep the delete
            mMod.erase(index);
            mDelete[index] = entry;
        }
    }

    void LedgerDelta::modEntry(EntryFrame::pointer& entry)
    {
        Hash index = entry->getIndex();
        auto mod_it = mMod.find(index);
        if ( mod_it != mMod.end())
        {
            // collapse mod
            mod_it->second = entry;
        }
        else
        {
            auto new_it = mNew.find(index);
            if (new_it != mNew.end())
            {
                // new + mod = new (with latest value)
                new_it->second = entry;
            }
            else
            {
                assert(mDelete.find(index) == mDelete.end()); // delete + mod is illegal
            }
        }
    }

    void LedgerDelta::merge(LedgerDelta &other)
    {
        for (auto &d : other.mDelete)
        {
            deleteEntry(d.second);
        }
        for (auto &n : other.mNew)
        {
            addEntry(n.second);
        }
        for (auto &m : other.mMod)
        {
            modEntry(m.second);
        }
    }

    //////////////////////////////////////////////////////////////////////////

    
}
