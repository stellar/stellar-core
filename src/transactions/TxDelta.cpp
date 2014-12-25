#include "transactions/TxDelta.h"
#include "lib/json/json.h"
#include "ledger/LedgerMaster.h"

namespace stellar
{

const char *TxDelta::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS TxDelta (						\
	txID		CHARACTER(35) PRIMARY KEY,	\
    ledgerSeq   INT UNSIGNED, \
	json	    BLOB \
);";

void TxDelta::merge(const TxDelta& other)
{
    // TODO.2
}

void TxDelta::setFinal(LedgerEntry& entry)
{
    auto it = mStartEnd.find(entry.getIndex());
    if(it == mStartEnd.end())
    {
        mStartEnd[entry.getIndex()] = std::pair<LedgerEntry::pointer, LedgerEntry::pointer>(LedgerEntry::pointer(), entry.copy());
    } else
    {
        it->second.second = entry.copy();
    }
}

void TxDelta::setStart(LedgerEntry& entry)
{
    auto it = mStartEnd.find(entry.getIndex());
    if(it == mStartEnd.end())
    {
        mStartEnd[entry.getIndex()] = std::pair<LedgerEntry::pointer, LedgerEntry::pointer>(entry.copy(), LedgerEntry::pointer());
    } else
    {
        it->second.first = entry.copy();
    }

}


void TxDelta::commitDelta(Json::Value& txResult, LedgerDelta& ledgerDelta, LedgerMaster& ledgerMaster)
{
    // run through every value of the start and end and make the correct SQL 
    for(auto pair : mStartEnd)
    {
        if(!pair.second.first)
        { // new entry
            pair.second.second->storeAdd(txResult, ledgerMaster);
            ledgerDelta.addEntry(pair.second.second);
        } else if(!pair.second.second)
        { // delete entry
            pair.second.first->storeDelete(txResult, ledgerMaster);
            ledgerDelta.deleteEntry(pair.second.first);
        } else
        {
            pair.second.second->storeChange(pair.second.first, txResult, ledgerMaster);
            ledgerDelta.modEntry(pair.second.second);
        }
    }   

    // save the Json in the DB
    std::stringstream json;
    json << txResult;
    ledgerMaster.getDatabase().getSession() <<
        "INSERT INTO TxDelta (txID,ledgerSeq,json) values (:v1,:v2,:v3)",
        soci::use(txResult["id"].asString()), soci::use(txResult["ledger"].asInt()),
        soci::use(json.str());
}
}