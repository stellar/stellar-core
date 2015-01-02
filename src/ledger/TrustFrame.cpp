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
		currency CHARACTER(35),				\
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
        hasher.add(mEntry.trustLine().issuer);
        hasher.add(mEntry.trustLine().currencyCode);
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
            txResult["effects"]["mod"][base58ID]["limit"] = mEntry.trustLine().limit;
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
        std::string b58Issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().issuer);
        std::string b58Currency = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().currencyCode);

        ledgerMaster.getDatabase().getSession() << "INSERT into TrustLines (trustIndex,accountID,issuer,currency,tlimit,authorized) values (:v1,:v2,:v3,:v4,:v5,:v6)",
            soci::use(base58Index), soci::use(b58AccountID), soci::use(b58Issuer),
            soci::use(b58Currency), soci::use(mEntry.trustLine().limit), soci::use(mEntry.trustLine().authorized);

        txResult["effects"]["new"][base58Index]["type"] = "trust";
        txResult["effects"]["new"][base58Index]["accountID"] = b58AccountID;
        txResult["effects"]["new"][base58Index]["issuer"] = b58Issuer;
        txResult["effects"]["new"][base58Index]["currency"] = b58Currency;
        txResult["effects"]["new"][base58Index]["limit"] = (Json::Int64)mEntry.trustLine().limit;
        txResult["effects"]["new"][base58Index]["authorized"] = mEntry.trustLine().authorized;
    }

    void TrustFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS TrustLines;";
        db.getSession() << kSQLCreateStatement;
    }


    /* 
    // NICOLAS -- code below does not work
    // need to rethink the way we store those lines:
    //     balance, low and high limit share the same currency
    //     should save/load the currency properly
    //     currency's issuer should be also first classed if we're going this direction



	TrustLine::TrustLine()
	{

	}

	TrustLine::TrustLine(SLE::pointer sle)
	{
		mLowLimit = sle->getFieldAmount(sfLowLimit);
		mHighLimit = sle->getFieldAmount(sfHighLimit);

        mBalance = sle->getFieldAmount(sfBalance);

		mLowAccount = mLowLimit.getIssuer();
		mHighAccount = mHighLimit.getIssuer();
		mCurrency = mLowLimit.getCurrency();


		uint32_t flags = sle->getFlags();

		mLowAuthSet = flags & lsfLowAuth;  // if the high account has authorized the low account to hold its credit?
		mHighAuthSet = flags & lsfHighAuth;
	}

	void TrustLine::calculateIndex()
	{
		Serializer  s(62);

		s.add16(spaceRipple);          //  2
		s.add160(mLowAccount); // 20
		s.add160(mHighAccount); // 20
		s.add160(mCurrency);           // 20
		mIndex = s.getSHA512Half();
	}


	// 
	TxResultCode TrustLine::fromTx(AccountEntry& signingAccount, TrustSetTx* tx)
	{	
		mCurrency=tx->mCurrency;
				
		if(signingAccount.mAccountID > tx->mOtherAccount)
		{
			mHighAccount = signingAccount.mAccountID;
			mLowAccount = tx->mOtherAccount;
		}else
		{
			mLowAccount = signingAccount.mAccountID;
			mHighAccount = tx->mOtherAccount;
		}
		
		if(tx->mAuthSet && !signingAccount.checkFlag(lsfRequireAuth))
		{
			Log(lsTRACE) <<	"Retry: Auth not required.";
			return tefNO_AUTH_REQUIRED;
		}
		
		return(tesSUCCESS);
	}

	bool TrustLine::loadFromDB(const uint256& index)
	{
		mIndex = index;
		std::string sql = "SELECT * FROM TrustLines WHERE trustIndex=x'";
		sql.append(to_string(index));
		sql.append("';");

		{
			DeprecatedScopedLock sl(getApp().getLedgerDB()->getDBLock());
			Database* db = getApp().getLedgerDB()->getDB();

			if (!db->executeSQL(sql, true) || !db->startIterRows())
				return false;

			mCurrency = db->getBigInt("currency");
			//mBalance = db->getBigInt("balance");
			mLowAccount = db->getAccountID("lowAccount");
			mHighAccount = db->getAccountID("highAccount");

			//mLowLimit = db->getBigInt("lowLimit");
			//mHighLimit = db->getBigInt("highLimit");
			mLowAuthSet = db->getBool("lowAuthSet");
			mHighAuthSet = db->getBool("highAuthSet");
		}

		return(true);
	}

	void TrustLine::insertIntoDB()
	{
		//make sure it isn't already in DB
		deleteFromDB();

		string sql = str(boost::format("INSERT INTO TrustLines (trustIndex, lowAccount,highAccount,lowLimit,highLimit,currency,balance,lowAuthSet,highAuthSet) values (x'%s','%s','%s','%s','%s','%s','%s',%d,%d);")
			% to_string(getIndex())
			% mLowAccount.base58Encode(RippleAddress::VER_ACCOUNT_ID)
			% mHighAccount.base58Encode(RippleAddress::VER_ACCOUNT_ID)
            % mLowLimit.getText()
			% mHighLimit.getText()
            % mCurrency.base58Encode(RippleAddress::VER_ACCOUNT_ID)
			% mBalance.getText()
			% mLowAuthSet
			% mHighAuthSet);

		{
			DeprecatedScopedLock sl(getApp().getLedgerDB()->getDBLock());
			Database* db = getApp().getLedgerDB()->getDB();

			if(!db->executeSQL(sql, true))
			{
				CLOG(WARNING, ripple::Ledger) << "SQL failed: " << sql;
			}
		}
	}

	void TrustLine::updateInDB()
	{
		string sql = str(boost::format("UPDATE TrustLines set lowLimit='%s' ,highLimit='%s' ,balance='%s' ,lowAuthSet=%d ,highAuthSet=%d where trustIndex=x'%s';")
            % mLowLimit.getText()
            % mHighLimit.getText()
            % mBalance.getText()
			% mLowAuthSet
			% mHighAuthSet
			% to_string(getIndex()));

		{
			DeprecatedScopedLock sl(getApp().getLedgerDB()->getDBLock());
			Database* db = getApp().getLedgerDB()->getDB();

			if(!db->executeSQL(sql, true))
			{
				CLOG(WARNING, ripple::Ledger) << "SQL failed: " << sql;
			}
		}
	}

	void TrustLine::deleteFromDB()
	{
		std::string sql = "DELETE FROM TrustLines where trustIndex=x'";
		sql.append(to_string(getIndex()));
		sql.append("';");

		{
			DeprecatedScopedLock sl(getApp().getLedgerDB()->getDBLock());
			Database* db = getApp().getLedgerDB()->getDB();

			if(!db->executeSQL(sql, true))
			{
				CLOG(WARNING, ripple::Ledger) << "SQL failed: " << sql;
			}
		}
	}

   
    */
}
