#ifndef __ACCOUNTENTRY__
#define __ACCOUNTENTRY__

#include "transactions/TransactionResultCodes.h"
#include "LedgerEntry.h"
#include "lib/crypto/StellarPublicKey.h"

namespace stellar
{
	class AccountEntry : public LedgerEntry
	{
		void calculateIndex();

		void insertIntoDB();
		void updateInDB();
		void deleteFromDB();

		//void serialize(stellarxdr::uint256& hash, SLE::pointer& ret);
	public:
        stellarxdr::uint160 mAccountID;
		uint64_t mBalance;
		uint32_t mSequence;
		uint32_t mOwnerCount;
		uint32_t mTransferRate;
		stellarxdr::uint160 mInflationDest;
		StellarPublicKey mPubKey; // SANITY make this optional and map to nullable in SQL
		bool mRequireDest;
		bool mRequireAuth;


		AccountEntry();
		AccountEntry(stellarxdr::uint160& id);

		bool loadFromDB(stellarxdr::uint256& index);
		bool loadFromDB(); // load by accountID

		//bool checkFlag(LedgerSpecificFlags flag);

		// will return txSUCCESS or that this account doesn't have the reserve to do this
		TxResultCode tryToIncreaseOwnerCount();

        static void dropAll(LedgerDatabase &db);
        static const char *kSQLCreateStatement;
	};
}

#endif

