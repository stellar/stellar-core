#ifndef __ACCOUNTENTRY__
#define __ACCOUNTENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/TxResultCode.h"
#include "ledger/EntryFrame.h"

namespace stellar
{
	class AccountFrame : public EntryFrame
	{
		void calculateIndex();
	public:
        typedef std::shared_ptr<AccountFrame> pointer;

        enum Flags
        {
            DISABLE_MASTER_FLAG = 1,
            DT_REQUIRED_FLAG = 2,
            AUTH_REQUIRED_FLAG = 4
        };
        
        AccountFrame();
        AccountFrame(LedgerEntry const& from);
        AccountFrame(uint256 const& id);

        EntryFrame::pointer copy()  const  { return EntryFrame::pointer(new AccountFrame(*this)); }


        uint64_t getBalance();
        bool isAuthRequired();
        uint256& getID();
        uint32_t getHighThreshold();
        uint32_t getMidThreshold();
        uint32_t getLowThreshold();
        uint32_t getSeqNum();

        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);
	

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement1;
        static const char *kSQLCreateStatement2;
        static const char *kSQLCreateStatement3;
	};
}

#endif

