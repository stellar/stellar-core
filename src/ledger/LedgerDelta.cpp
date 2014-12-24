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

    void TxDelta::merge(const TxDelta& other)
    {

    }

    void TxDelta::setFinal(const LedgerEntry& entry)
    {
        auto it = mStartEnd.find(entry.getIndex());
        if(it == mStartEnd.end())
        {
            mStartEnd[entry.getIndex()] = std::pair<LedgerEntry::pointer, LedgerEntry::pointer>(LedgerEntry::pointer(), entry.copy());
        } else
        {
            it->second.second = entry;
        }
    }

    void TxDelta::setStart(const LedgerEntry& entry)
    {
        auto it = mStartEnd.find(entry.getIndex());
        if(it == mStartEnd.end())
        {
            mStartEnd[entry.getIndex()] = std::pair<LedgerEntry::pointer, LedgerEntry::pointer>(entry.copy(), LedgerEntry::pointer());
        } else
        {
            it->second.first = entry;
        }
        
    }


    void TxDelta::commitDelta(Json::Value& txResult, LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        // TODO.2  save the txResult in the DB
        // run through every value of the start and end and make the correct SQL
        // TODO.2

        for(auto pair : mStartEnd)
        {
            if(!pair.second.first)
            { // new entry

            } else if(!pair.second.second)
            { // delete entry
                sql << "DELETE from " << pair.second->getTableName() << " where "
            } else
            {

                sql << "UPDATE " << pair.second->getTableName() << " set "
            }
        }
    }
}
