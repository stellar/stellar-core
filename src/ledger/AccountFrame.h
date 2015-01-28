#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"

namespace stellar
{
	class AccountFrame : public EntryFrame
	{
		void calculateIndex();
        void storeUpdate(LedgerDelta &delta, LedgerMaster& ledgerMaster, bool insert);
        bool mUpdateSigners;

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

        void setUpdateSigners() { mUpdateSigners = true; }
        int64_t getBalance();
        bool isAuthRequired();
        uint256 const& getID() const;
        uint32_t getMasterWeight();
        uint32_t getHighThreshold();
        uint32_t getMidThreshold();
        uint32_t getLowThreshold();
        uint32_t getSeqNum();

        void storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster);
        void storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster);
        void storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster);
	

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement1;
        static const char *kSQLCreateStatement2;
        static const char *kSQLCreateStatement3;
	};
}



