#ifndef __TRUSTLINE__
#define __TRUSTLINE__


#include "ledger/LedgerEntry.h"
#include "transactions/TransactionResultCodes.h"

namespace stellar {

	class TrustSetTx;
	class AccountEntry;

	//index is: 
	class TrustLine : public LedgerEntry
	{
		void calculateIndex();
		
		void insertIntoDB();
		void updateInDB();
		void deleteFromDB();

		// void serialize(stellarxdr::uint256& hash, SLE::pointer& ret);

	public:
        stellarxdr::uint160 mLowAccount;
        stellarxdr::uint160 mHighAccount;
        stellarxdr::uint160 mCurrency;

		//STAmount mLowLimit;
		//STAmount mHighLimit;
		//STAmount mBalance;	// NICOLAS: check this  positive balance means credit is held by high account

		bool mLowAuthSet;  // if the high account has authorized the low account to hold its credit
		bool mHighAuthSet;

		TrustLine();
		//TrustLine(SLE::pointer sle);

		bool loadFromDB(const stellarxdr::uint256& index);
		TxResultCode fromTx(AccountEntry& signingAccount, TrustSetTx* tx);
		
        static void dropAll(LedgerDatabase &db);
        static const char *kSQLCreateStatement;
		
	};
}

#endif
