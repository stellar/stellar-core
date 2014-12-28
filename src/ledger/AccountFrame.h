#ifndef __ACCOUNTENTRY__
#define __ACCOUNTENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/TxResultCode.h"
#include "ledger/EntryFrame.h"
#include "crypto/StellarPublicKey.h"

namespace stellar
{
	class AccountFrame : public EntryFrame
	{
		void calculateIndex();

		
		//void serialize(uint256& hash, SLE::pointer& ret);
	public:
        typedef std::shared_ptr<AccountFrame> pointer;

        enum Flags
        {
            DISABLE_MASTER_FLAG = 1,
            DT_REQUIRED_FLAG = 2,
            AUTH_REQUIRED_FLAG = 4
        };
        
        AccountFrame();
        AccountFrame(const LedgerEntry& from);
        AccountFrame(uint256& id);

        EntryFrame::pointer copy()  const  { return EntryFrame::pointer(new AccountFrame(*this)); }


        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);

        bool authRequired();

		// will return txSUCCESS or that this account doesn't have the reserve to do this
		TxResultCode tryToIncreaseOwnerCount();

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}

#endif

