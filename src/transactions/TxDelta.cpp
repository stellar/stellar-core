#include "transactions/TxDelta.h"
#include "lib/json/json.h"
#include "ledger/LedgerMaster.h"
#include "database/Database.h"

/*

txResult['id']=
txResult['code']=
txResult['effects']['delete']=
txResult['effects']['new']=
txResult['effects']['mod']=

result
    TxID
    result code
    effects
        delete
            EntryID
            ...
        new
            EntryXDR
            ...
        mod
            EntryID
            fieldName : new value
            ...
        ...

*/


namespace stellar
{

const char *TxDelta::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS TxDelta (						\
	txID		CHARACTER(35),	\
    ledgerSeq   INT UNSIGNED, \
	json	    BLOB \
);";

void TxDelta::merge(const TxDelta& other)
{
    for(auto item : other.mStartEnd)
    {
        if(item.second.first) setStart(*item.second.first);
        if(item.second.second) setFinal(*item.second.second);
    }
}

void TxDelta::setFinal(EntryFrame& entry)
{
    auto it = mStartEnd.find(entry.getIndex());
    if(it == mStartEnd.end())
    {
        StartEndPair pair;
        pair.second = entry.copy();
        mStartEnd[entry.getIndex()] = pair;
    } else
    {
        it->second.second = entry.copy();
    }
}

void TxDelta::setStart(EntryFrame&  entry)
{
    auto it = mStartEnd.find(entry.getIndex());
    if(it == mStartEnd.end())
    {
        StartEndPair pair;
        pair.first = entry.copy();
        mStartEnd[entry.getIndex()] = pair;
    } else
    {
        it->second.first = entry.copy();
    }
}

void TxDelta::removeFinal(EntryFrame& entry)
{
    auto it = mStartEnd.find(entry.getIndex());
    if(it != mStartEnd.end())
    { 
        StartEndPair pair;
        pair.first = it->second.first;
        mStartEnd[entry.getIndex()] = pair;
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
            ledgerDelta.addEntry(*pair.second.second);
        } else if(!pair.second.second)
        { // delete entry
            pair.second.first->storeDelete(txResult, ledgerMaster);
            ledgerDelta.deleteEntry(*pair.second.first);
        } else
        {
            pair.second.second->storeChange(pair.second.first, txResult, ledgerMaster);
            ledgerDelta.modEntry(*pair.second.second);
        }
    }   

    // save the Json in the DB
    std::stringstream json;
    json << txResult;

    std::string txID(txResult["id"].asString()), jsonStr(json.str());

    ledgerMaster.getDatabase().getSession() <<
        "INSERT INTO TxDelta (txID,ledgerSeq,json) values (:v1,:v2,:v3)",
        soci::use(txID), soci::use(txResult["ledger"].asInt()),
        soci::use(jsonStr);
}

void TxDelta::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS TxDelta;";
    db.getSession() << kSQLCreateStatement;
}

}
