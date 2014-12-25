#ifndef __ACCOUNTENTRY__
#define __ACCOUNTENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/TxResultCode.h"
#include "LedgerEntry.h"
#include "crypto/StellarPublicKey.h"

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
        typedef std::shared_ptr<AccountEntry> pointer;
        
		AccountEntry(const stellarxdr::LedgerEntry& from);
        AccountEntry(stellarxdr::uint160& id);

        LedgerEntry::pointer copy()  const  { return LedgerEntry::pointer(new AccountEntry(*this)); }


        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(LedgerEntry::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);

		// will return txSUCCESS or that this account doesn't have the reserve to do this
		TxResultCode tryToIncreaseOwnerCount();

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}

#endif

