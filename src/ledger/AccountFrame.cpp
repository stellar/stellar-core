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
		sequence		INT UNSIGNED default 1,				\
		ownerCount		INT UNSIGNED default 0,			\
		transferRate	INT UNSIGNED default 0,		\
        publicKey   	CHARACTER(35),		\
        inflationDest	CHARACTER(35),		\
		creditAuthKey	CHARACTER(56),		\
		flags		    INT UNSIGNED default 0  	\
	);";

    AccountFrame::AccountFrame()
    {
        mEntry.type(ACCOUNT);
    }

    AccountFrame::AccountFrame(const LedgerEntry& from) : EntryFrame(from)
    {

    }

    AccountFrame::AccountFrame(uint256& id)
    {
        mEntry.type(ACCOUNT);
        mEntry.account().accountID = id;
        mEntry.account().sequence = 1;
    }

    void AccountFrame::calculateIndex()
    {
        mIndex = mEntry.account().accountID;
    }

    bool AccountFrame::authRequired()
    {
        return(mEntry.account().flags & AccountFrame::AUTH_REQUIRED_FLAG);
    }

    uint64_t AccountFrame::getBalance()
    {
        return(mEntry.account().balance);
    }

    void AccountFrame::storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        txResult["effects"]["delete"][base58ID];

        ledgerMaster.getDatabase().getSession() << 
            "DELETE from Accounts where accountID= :v1", soci::use(base58ID);
    }

    void AccountFrame::storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster)
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

        txResult["effects"]["new"][base58ID]["balance"] = (Json::Int64)mEntry.account().balance;
    }

    void AccountFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS Accounts;";
        db.getSession() << kSQLCreateStatement;
        

    }

   

    /*
    

	
	AccountEntry::AccountEntry(SLE::pointer sle)
	{
		mAccountID=sle->getFieldAccount160(sfAccount);
		mBalance = sle->getFieldAmount(sfBalance).getNValue();
		mSequence=sle->getFieldU32(sfSequence);
		mOwnerCount=sle->getFieldU32(sfOwnerCount);
		if(sle->isFieldPresent(sfTransferRate))
			mTransferRate=sle->getFieldU32(sfTransferRate);
		else mTransferRate = 0;

		if(sle->isFieldPresent(sfInflationDest))
			mInflationDest=sle->getFieldAccount160(sfInflationDest);
		
		uint32_t flags = sle->getFlags();

		mRequireDest = flags & lsfRequireDestTag;
		mRequireAuth = flags & lsfRequireAuth;

		// if(sle->isFieldPresent(sfPublicKey)) 
		//	mPubKey=
	}

	void AccountEntry::calculateIndex()
	{
		Serializer  s(22);

		s.add16(spaceAccount); //  2
		s.add160(mAccountID);  // 20

		mIndex= s.getSHA512Half();
	}

	void  AccountEntry::insertIntoDB()
	{
		//make sure it isn't already in DB
		deleteFromDB();

		string sql = str(boost::format("INSERT INTO Accounts (accountID,balance,sequence,owenerCount,transferRate,inflationDest,publicKey,requireDest,requireAuth) values ('%s',%d,%d,%d,%d,'%s','%s',%d,%d);")
			% mAccountID.base58Encode(RippleAddress::VER_ACCOUNT_ID)
			% mBalance
			% mSequence
			% mOwnerCount
			% mTransferRate
			% mInflationDest.base58Encode(RippleAddress::VER_ACCOUNT_ID)
			% mPubKey.base58Key()
			% mRequireDest
			% mRequireAuth);

		Database* db = getApp().getWorkingLedgerDB()->getDB();

		if(!db->executeSQL(sql, true))
		{
			CLOG(WARNING, "Ledger") << "SQL failed: " << sql;
		}
	}
	void AccountEntry::updateInDB()
	{
		string sql = str(boost::format("UPDATE Accounts set balance=%d, sequence=%d,owenerCount=%d,transferRate=%d,inflationDest='%s',publicKey='%s',requireDest=%d,requireAuth=%d where accountID='%s';")
			% mBalance
			% mSequence
			% mOwnerCount
			% mTransferRate
			% mInflationDest.base58Encode(RippleAddress::VER_ACCOUNT_ID)
			% mPubKey.base58Key()
			% mRequireDest
			% mRequireAuth
			% mAccountID.base58Encode(RippleAddress::VER_ACCOUNT_ID));

		Database* db = getApp().getWorkingLedgerDB()->getDB();

		if(!db->executeSQL(sql, true))
		{
			CLOG(WARNING, ripple::Ledger) << "SQL failed: " << sql;
		}
	}
	void AccountEntry::deleteFromDB()
	{
		string sql = str(boost::format("DELETE from Accounts where accountID='%s';")
			% mAccountID.base58Encode(RippleAddress::VER_ACCOUNT_ID));

		Database* db = getApp().getWorkingLedgerDB()->getDB();

		if(!db->executeSQL(sql, true))
		{
			CLOG(WARNING, ripple::Ledger) << "SQL failed: " << sql;
		}
	}

	bool AccountEntry::checkFlag(LedgerSpecificFlags flag)
	{
		
		return(true);
	}

	// will return tesSUCCESS or that this account doesn't have the reserve to do this
	TxResultCode AccountEntry::tryToIncreaseOwnerCount()
	{
		// The reserve required to create the line.
		uint64_t reserveNeeded = gLedgerMaster->getCurrentLedger()->getReserve(mOwnerCount + 1);

		if(mBalance >= reserveNeeded)
		{
			mOwnerCount++;
			return tesSUCCESS;
		}
		return tecINSUF_RESERVE_LINE;
	}

    
    */
}

