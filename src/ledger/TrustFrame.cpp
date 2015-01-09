// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/TrustFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerMaster.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"

using namespace std;

namespace stellar {
    const char *TrustFrame::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS TrustLines (					\
		trustIndex CHARACTER(35) PRIMARY KEY,				\
		accountID	CHARACTER(35),			\
		issuer CHARACTER(35),				\
		isoCurrency CHARACTER(4),    		\
		tlimit UNSIGNED INT,		   		\
		balance UNSIGNED INT,				\
		authorized BOOL						\
	); ";

    TrustFrame::TrustFrame()
    {

    }
    TrustFrame::TrustFrame(const LedgerEntry& from) : EntryFrame(from)
    {

    }

    
    void TrustFrame::calculateIndex()
    {
        // hash of accountID+issuer+currency
        SHA512_256 hasher;
        hasher.add(mEntry.trustLine().accountID);
        hasher.add(mEntry.trustLine().currency.isoCI().issuer);
        hasher.add(mEntry.trustLine().currency.isoCI().currencyCode);
        mIndex = hasher.finish();
    }

    void TrustFrame::storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        txResult["effects"]["delete"][base58ID];

        ledgerMaster.getDatabase().getSession() <<
            "DELETE from TrustLines where trustIndex= :v1", soci::use(base58ID);
    }
    void TrustFrame::storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        std::stringstream sql;
        sql << "UPDATE TrustLines set ";

        bool before = false;

        if(mEntry.trustLine().balance != startFrom->mEntry.trustLine().balance)
        {
            sql << " balance= " << mEntry.trustLine().balance;
            txResult["effects"]["mod"][base58ID]["balance"] = (Json::Int64)mEntry.trustLine().balance;

            before = true;
        }

        if(mEntry.trustLine().limit != startFrom->mEntry.trustLine().limit)
        {
            if(before) sql << ", ";
            sql << " tlimit= " << mEntry.trustLine().limit;
            txResult["effects"]["mod"][base58ID]["limit"] = (Json::Int64)mEntry.trustLine().limit;
            before = true;
        }

        if(mEntry.trustLine().authorized != startFrom->mEntry.trustLine().authorized)
        {
            if(before) sql << ", ";
            sql << " authorized= " << mEntry.trustLine().authorized;
            txResult["effects"]["mod"][base58ID]["authorized"] = mEntry.trustLine().authorized;
           
        }

        sql << " where trustIndex='" << base58ID << "';";
        ledgerMaster.getDatabase().getSession() << sql.str();
    }

    void TrustFrame::storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster)
    {
        std::string base58Index = toBase58Check(VER_ACCOUNT_ID, getIndex());
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().accountID);
        std::string b58Issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().currency.isoCI().issuer);
        std::string currencyCode;
        currencyCodeToStr(mEntry.trustLine().currency.isoCI().currencyCode,currencyCode);

        ledgerMaster.getDatabase().getSession() << "INSERT into TrustLines (trustIndex,accountID,issuer,currency,tlimit,authorized) values (:v1,:v2,:v3,:v4,:v5,:v6)",
            soci::use(base58Index), soci::use(b58AccountID), soci::use(b58Issuer),
            soci::use(currencyCode), soci::use(mEntry.trustLine().limit),
            soci::use((int)mEntry.trustLine().authorized);

        txResult["effects"]["new"][base58Index]["type"] = "trust";
        txResult["effects"]["new"][base58Index]["accountID"] = b58AccountID;
        txResult["effects"]["new"][base58Index]["issuer"] = b58Issuer;
        txResult["effects"]["new"][base58Index]["currency"] = currencyCode;
        txResult["effects"]["new"][base58Index]["limit"] = (Json::Int64)mEntry.trustLine().limit;
        txResult["effects"]["new"][base58Index]["authorized"] = mEntry.trustLine().authorized;
    }

    void TrustFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS TrustLines;";
        db.getSession() << kSQLCreateStatement;
    }
}
