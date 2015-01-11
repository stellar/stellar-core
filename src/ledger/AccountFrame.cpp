// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "AccountFrame.h"
#include "LedgerMaster.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "database/Database.h"

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
    thresholds   	BLOB(4),		            \
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
    mEntry.account().thresholds[0] = 1; // by default, master key's weight is 1
}

void AccountFrame::calculateIndex()
{
    mIndex = mEntry.account().accountID;
}

bool AccountFrame::isAuthRequired()
{
    return(mEntry.account().flags & AccountFrame::AUTH_REQUIRED_FLAG);
}

uint32_t AccountFrame::getSeqNum()
{
    return(mEntry.account().sequence);
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

void AccountFrame::storeDelete(rapidjson::Value& txResult, LedgerMaster& ledgerMaster)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    txResult["effects"]["delete"][base58ID.c_str()];

    ledgerMaster.getDatabase().getSession() << 
        "DELETE from Accounts where accountID= :v1", soci::use(base58ID);
    ledgerMaster.getDatabase().getSession() <<
        "DELETE from AccountData where accountID= :v1", soci::use(base58ID);
    ledgerMaster.getDatabase().getSession() <<
        "DELETE from Signers where accountID= :v1", soci::use(base58ID);
}

void AccountFrame::storeUpdate(EntryFrame::pointer startFrom, rapidjson::Value& txResult,
    LedgerMaster& ledgerMaster, bool insert)
{
    AccountEntry& finalAccount = mEntry.account();
    AccountEntry& startAccount = startFrom->mEntry.account();
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    std::stringstream sql;

    const char * op = insert ? "new" : "mod";

    if (insert)
    {
        sql << "INSERT INTO Accounts ( accountID, balance, sequence,    \
            ownerCount, transferRate, inflationDest, thresholds, flags) \
            VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7 )";
    }
    else
    {
        sql << "UPDATE Accounts SET balance = :v1, sequence = :v2, ownerCount = :v3, \
                transferRate = :v4, inflationDest = :v5, thresholds = :v6, \
                flags = :v7 WHERE accountID = :id";
    }

    if(insert || finalAccount.balance != startAccount.balance)
    { 
        txResult["effects"][op][base58ID.c_str()]["balance"] = finalAccount.balance;
    }

    if(insert || finalAccount.sequence != startAccount.sequence || insert)
    {
        txResult["effects"][op][base58ID.c_str()]["sequence"] = finalAccount.sequence;
    }

    if(insert || finalAccount.transferRate != startAccount.transferRate)
    {
        txResult["effects"][op][base58ID.c_str()]["transferRate"] = finalAccount.transferRate;
    }

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStr;

    if(finalAccount.inflationDest)
    {
        if(insert || !startAccount.inflationDest || *finalAccount.inflationDest != *startAccount.inflationDest)
        {
            inflationDestStr = toBase58Check(VER_ACCOUNT_PUBLIC, *finalAccount.inflationDest);
            inflation_ind = soci::i_ok;
            txResult["effects"][op][base58ID.c_str()]["inflationDest"] = inflationDestStr.c_str();
        }
    }
    
    if(insert || finalAccount.thresholds != startAccount.thresholds)
    {
        txResult["effects"][op][base58ID.c_str()]["thresholds"]['0'] = finalAccount.thresholds[0];
        txResult["effects"][op][base58ID.c_str()]["thresholds"]['1'] = finalAccount.thresholds[1];
        txResult["effects"][op][base58ID.c_str()]["thresholds"]['2'] = finalAccount.thresholds[2];
        txResult["effects"][op][base58ID.c_str()]["thresholds"]['3'] = finalAccount.thresholds[3];
    }
    

    if(insert || mEntry.account().flags != startAccount.flags)
    {
        txResult["effects"][op][base58ID.c_str()]["flags"] = finalAccount.flags;
    }

    // TODO.3   KeyValue data
    ledgerMaster.getDatabase().getSession() << sql.str(), use(base58ID, "id"),
        use(finalAccount.balance, "v1"), use(finalAccount.sequence, "v2"),
        use(finalAccount.ownerCount, "v3"), use(finalAccount.transferRate, "v4"),
        use(inflationDestStr, inflation_ind, "v5"),
        use(binToHex(finalAccount.thresholds), "v6"), use(finalAccount.flags, "v7");

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
                        txResult["effects"]["mod"][base58ID.c_str()]["signers"][b58signKey.c_str()] = finalSigner.weight;
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
                txResult["effects"]["mod"][base58ID.c_str()]["signers"][b58signKey.c_str()] = 0;
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
                        txResult["effects"][op][base58ID.c_str()]["signers"][b58signKey.c_str()] = finalSigner.weight;
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
                txResult["effects"]["new"][base58ID.c_str()]["signers"][b58signKey.c_str()] = finalSigner.weight;
                ledgerMaster.getDatabase().getSession() << "INSERT INTO Signers (accountID,pubKey,weight) values (:v1,:v2,:v3)",
                    use(base58ID), use(b58signKey), use(finalSigner.weight);
            }
        }
    }
}

void AccountFrame::storeChange(EntryFrame::pointer startFrom,
    rapidjson::Value& txResult, LedgerMaster& ledgerMaster)
{
    storeUpdate(startFrom, txResult, ledgerMaster, false);
}

void AccountFrame::storeAdd(rapidjson::Value& txResult, LedgerMaster& ledgerMaster)
{
    EntryFrame::pointer emptyAccount = make_shared<AccountFrame>();
    storeUpdate(emptyAccount, txResult, ledgerMaster, true);
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

