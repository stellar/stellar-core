// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "AccountFrame.h"
#include "LedgerMaster.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"

namespace stellar
{
    const char *AccountFrame::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Accounts (						\
		accountID		CHARACTER(35) PRIMARY KEY,	\
		balance			BIGINT UNSIGNED,			\
		sequence		INT UNSIGNED default 1,		\
		ownerCount		INT UNSIGNED default 0,		\
		transferRate	INT UNSIGNED default 0,		\
        publicKey   	CHARACTER(35),		        \
        inflationDest	CHARACTER(35),		        \
		creditAuthKey	CHARACTER(56),		        \
		flags		    INT UNSIGNED default 0  	\
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

    void AccountFrame::storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        txResult["effects"]["delete"][base58ID];

        ledgerMaster.getDatabase().getSession() << 
            "DELETE from Accounts where accountID= :v1", soci::use(base58ID);
    }

    void AccountFrame::storeChange(EntryFrame::pointer startFrom, 
        Json::Value& txResult, LedgerMaster& ledgerMaster)
    {  
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        std::stringstream sql;
        sql << "UPDATE Accounts set ";

        bool before = false;

        if(mEntry.account().balance != startFrom->mEntry.account().balance)
        { 
            sql << " balance= " << mEntry.account().balance;
            txResult["effects"]["mod"][base58ID]["balance"] = (Json::Int64)mEntry.account().balance;
            
            before = true;
        }
        
        if(mEntry.account().sequence != startFrom->mEntry.account().sequence)
        {
            if(before) sql << ", ";
            sql << " sequence= " << mEntry.account().sequence;
            txResult["effects"]["mod"][base58ID]["sequence"] = mEntry.account().sequence;
            before = true;
        }

        if(mEntry.account().transferRate != startFrom->mEntry.account().transferRate)
        {
            if(before) sql << ", ";
            sql << " transferRate= " << mEntry.account().transferRate;
            txResult["effects"]["mod"][base58ID]["transferRate"] = mEntry.account().transferRate;
            before = true;
        }

        if(mEntry.account().pubKey != startFrom->mEntry.account().pubKey)
        {
            string keyStr = toBase58Check(VER_ACCOUNT_PUBLIC, *mEntry.account().pubKey);

            if(before) sql << ", ";
            sql << " pubKey= '" << keyStr << "' ";
            txResult["effects"]["mod"][base58ID]["pubKey"] = keyStr;
            before = true;
        }

        // TODO.2  make safe
        if(mEntry.account().inflationDest != startFrom->mEntry.account().inflationDest)
        {
            string keyStr = toBase58Check(VER_ACCOUNT_PUBLIC, *mEntry.account().inflationDest);
            if(before) sql << ", ";
            sql << " inflationDest= '" << keyStr << "' ";
            txResult["effects"]["mod"][base58ID]["inflationDest"] = keyStr;
            before = true;
        }

        if(mEntry.account().creditAuthKey != startFrom->mEntry.account().creditAuthKey)
        {
            string keyStr = toBase58Check(VER_ACCOUNT_PUBLIC, *mEntry.account().creditAuthKey);

            if(before) sql << ", ";
            sql << " creditAuthKey= '" << keyStr << "' " ;
            txResult["effects"]["mod"][base58ID]["creditAuthKey"] = keyStr;
            before = true;
        }

        if(mEntry.account().flags != startFrom->mEntry.account().flags)
        {
            if(before) sql << ", ";
            sql << " flags= " << mEntry.account().flags;
            txResult["effects"]["mod"][base58ID]["flags"] = mEntry.account().flags;
        }

        // TODO.3   KeyValue data
        sql << " where accountID='" << base58ID << "';";
        ledgerMaster.getDatabase().getSession() << sql.str();
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
        db.getSession() << kSQLCreateStatement;
    }
}

