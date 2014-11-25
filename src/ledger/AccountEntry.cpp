#include <boost/format.hpp>
#include "AccountEntry.h"
#include "LedgerMaster.h"

namespace stellar
{
    /* NICOLAS
    const char *AccountEntry::kSQLCreateStatement = 	"CREATE TABLE IF NOT EXISTS Accounts (						\
		accountID		CHARACTER(35) PRIMARY KEY,	\
		balance			BIGINT UNSIGNED,			\
		sequence		INT UNSIGNED,				\
		owenerCount		INT UNSIGNED,			\
		transferRate	INT UNSIGNED,		\
		inflationDest	CHARACTER(35),		\
		publicKey		CHARACTER(56),		\
		requireDest		BOOL,				\
		requireAuth		BOOL				\
	);";

	
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
			WriteLog(lsWARNING, ripple::Ledger) << "SQL failed: " << sql;
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
			WriteLog(lsWARNING, ripple::Ledger) << "SQL failed: " << sql;
		}
	}
	void AccountEntry::deleteFromDB()
	{
		string sql = str(boost::format("DELETE from Accounts where accountID='%s';")
			% mAccountID.base58Encode(RippleAddress::VER_ACCOUNT_ID));

		Database* db = getApp().getWorkingLedgerDB()->getDB();

		if(!db->executeSQL(sql, true))
		{
			WriteLog(lsWARNING, ripple::Ledger) << "SQL failed: " << sql;
		}
	}

	bool AccountEntry::checkFlag(LedgerSpecificFlags flag)
	{
		// TODO: 
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

    void AccountEntry::dropAll(LedgerDatabase &db)
    {
        if (!db.getDBCon()->getDB()->executeSQL("DROP TABLE IF EXISTS Accounts;"))
		{
            throw std::runtime_error("Could not drop Account data");
		}
        if (!db.getDBCon()->getDB()->executeSQL(kSQLCreateStatement))
        {
            throw std::runtime_error("Could not recreate Account data");
		}
    }
    */
}

