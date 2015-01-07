// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "AccountFrame.h"
#include "LedgerMaster.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"


using namespace soci;

namespace stellar
{
const char *AccountFrame::kSQLCreateStatement1 = "CREATE TABLE IF NOT EXISTS Accounts (						\
	accountID		CHARACTER(35) PRIMARY KEY,	\
	balance			BIGINT UNSIGNED,			\
	sequence		INT UNSIGNED default 1,		\
	ownerCount		INT UNSIGNED default 0,		\
	transferRate	INT UNSIGNED default 0,		\
    inflationDest	CHARACTER(35),		        \
    thresholds   	INT UNSIGNED default 0,		\
	flags		    INT UNSIGNED default 0  	\
);";

const char *AccountFrame::kSQLCreateStatement2 = "CREATE TABLE IF NOT EXISTS Signers (						\
	accountID		CHARACTER(35) PRIMARY KEY,	\
    publicKey   	CHARACTER(35),		        \
    weight	INT 	\
);";

const char *AccountFrame::kSQLCreateStatement3 = "CREATE TABLE IF NOT EXISTS AccountData (						\
	accountID		CHARACTER(35) PRIMARY KEY,	\
    key INT, \
	value			BLOB(32)			\
);";

AccountFrame::AccountFrame()
{
    mEntry.type(ACCOUNT);
}

AccountFrame::AccountFrame(LedgerEntry const& from) : EntryFrame(from)
{

}

AccountFrame::AccountFrame(uint256 const& id)
{
    mEntry.type(ACCOUNT);
    mEntry.account().accountID = id;
    mEntry.account().sequence = 1;
}

void AccountFrame::calculateIndex()
{
    mIndex = mEntry.account().accountID;
}

bool AccountFrame::isAuthRequired()
{
    return(mEntry.account().flags & AccountFrame::AUTH_REQUIRED_FLAG);
}

uint64_t AccountFrame::getBalance()
{
    return(mEntry.account().balance);
}
uint256& AccountFrame::getID()
{
    return(mEntry.account().accountID);
}
uint32_t AccountFrame::getHighThreshold()
{
    return mEntry.account().thresholds[3];
}
uint32_t AccountFrame::getMidThreshold()
{
    return mEntry.account().thresholds[2];
}
uint32_t AccountFrame::getLowThreshold()
{
    return mEntry.account().thresholds[1];
}

void AccountFrame::storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    txResult["effects"]["delete"][base58ID];

    ledgerMaster.getDatabase().getSession() << 
        "DELETE from Accounts where accountID= :v1", soci::use(base58ID);
    ledgerMaster.getDatabase().getSession() <<
        "DELETE from AccountData where accountID= :v1", soci::use(base58ID);
    ledgerMaster.getDatabase().getSession() <<
        "DELETE from Signers where accountID= :v1", soci::use(base58ID);
}

void AccountFrame::storeChange(EntryFrame::pointer startFrom, 
    Json::Value& txResult, LedgerMaster& ledgerMaster)
{  
    AccountEntry& finalAccount = mEntry.account();
    AccountEntry& startAccount = startFrom->mEntry.account();
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    std::stringstream sql;
    sql << "UPDATE Accounts set ";

    bool before = false;

    if(finalAccount.balance != startAccount.balance)
    { 
        sql << " balance= " << mEntry.account().balance;
        txResult["effects"]["mod"][base58ID]["balance"] = (Json::Int64)mEntry.account().balance;
            
        before = true;
    }
        
    if(mEntry.account().sequence != startAccount.sequence)
    {
        if(before) sql << ", ";
        sql << " sequence= " << mEntry.account().sequence;
        txResult["effects"]["mod"][base58ID]["sequence"] = mEntry.account().sequence;
        before = true;
    }

    if(mEntry.account().transferRate != startAccount.transferRate)
    {
        if(before) sql << ", ";
        sql << " transferRate= " << mEntry.account().transferRate;
        txResult["effects"]["mod"][base58ID]["transferRate"] = mEntry.account().transferRate;
        before = true;
    }


    if(finalAccount.inflationDest)
    {
        if(!startAccount.inflationDest || *finalAccount.inflationDest != *startAccount.inflationDest)
        {
            string keyStr = toBase58Check(VER_ACCOUNT_PUBLIC, *finalAccount.inflationDest);
            if(before) sql << ", ";
            sql << " inflationDest= '" << keyStr << "' ";
            txResult["effects"]["mod"][base58ID]["inflationDest"] = keyStr;
            before = true;
        }
    }
    /* TODO.2
    if(mEntry.account().thresholds != startAccount.thresholds)
    {
        if(before) sql << ", ";
        sql << " thresholds= " << mEntry.account().thresholds;
        txResult["effects"]["mod"][base58ID]["thresholds"] = mEntry.account().thresholds;
        before = true;
    }
    */

    if(mEntry.account().flags != startAccount.flags)
    {
        if(before) sql << ", ";
        sql << " flags= " << mEntry.account().flags;
        txResult["effects"]["mod"][base58ID]["flags"] = mEntry.account().flags;
    }

    // TODO.3   KeyValue data
    sql << " where accountID='" << base58ID << "';";
    ledgerMaster.getDatabase().getSession() << sql.str();

    // deal with changes to Signers
    if(finalAccount.signers.size() < startAccount.signers.size())
    { // some signers were removed
        for(auto startSigner : startAccount.signers)
        {
            bool found = false;
            for(auto finalSigner : finalAccount.signers)
            {
                if(finalSigner.pubKey == startSigner.pubKey)
                {
                    if(finalSigner.weight != startSigner.weight)
                    {
                        std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);
                        txResult["effects"]["mod"][base58ID]["signers"][b58signKey] = finalSigner.weight;
                        ledgerMaster.getDatabase().getSession() << "UPDATE Signers set weight=:v1 where accountID=:v2 and pubKey=:v3",
                            use(finalSigner.weight), use(base58ID), use(b58signKey);
                    }
                    found = true;
                    break;
                }
            }
            if(!found)
            { // delete signer
                std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, startSigner.pubKey);
                txResult["effects"]["mod"][base58ID]["signers"][b58signKey] = 0;
                ledgerMaster.getDatabase().getSession() << "DELETE from Signers where accountID=:v2 and pubKey=:v3",
                     use(base58ID), use(b58signKey);
            }
        }
    } else
    { // signers added or the same
        for(auto finalSigner : finalAccount.signers)
        {
            bool found = false;
            for(auto startSigner : startAccount.signers)
            {
                if(finalSigner.pubKey == startSigner.pubKey)
                {
                    if(finalSigner.weight != startSigner.weight)
                    {
                        std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);
                        txResult["effects"]["mod"][base58ID]["signers"][b58signKey] = finalSigner.weight;
                        ledgerMaster.getDatabase().getSession() << "UPDATE Signers set weight=:v1 where accountID=:v2 and pubKey=:v3",
                            use(finalSigner.weight), use(base58ID), use(b58signKey);
                    }
                    found = true;
                    break;
                }
            }
            if(!found)
            { // new signer
                std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);
                txResult["effects"]["mod"][base58ID]["signers"][b58signKey] = finalSigner.weight;
                ledgerMaster.getDatabase().getSession() << "INSERT INTO Signers (accountID,pubKey,weight) values (:v1,:v2,:v3)",
                    use(base58ID), use(b58signKey), use(finalSigner.weight);
            }
        }
    }
}


void AccountFrame::storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    ledgerMaster.getDatabase().getSession() << "INSERT into Accounts (accountID,balance) values (:v1,:v2)",
            soci::use(base58ID), soci::use(mEntry.account().balance);

    txResult["effects"]["new"][base58ID]["type"] = "account";
    txResult["effects"]["new"][base58ID]["balance"] = (Json::Int64)mEntry.account().balance;
}

void AccountFrame::dropAll(Database &db)
{
    db.getSession() << "DROP TABLE IF EXISTS Accounts;";
    db.getSession() << "DROP TABLE IF EXISTS Signers;";
    db.getSession() << "DROP TABLE IF EXISTS AccountData;";

    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
    db.getSession() << kSQLCreateStatement3;
}
}

