#ifndef __TRUSTLINE__
#define __TRUSTLINE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"
#include "transactions/TxResultCode.h"

namespace stellar {

	class TrustSetTx;
	

	//index is: 
	class TrustFrame : public EntryFrame
	{
		void calculateIndex();
		
	public:
        typedef std::shared_ptr<TrustFrame> pointer;

        TrustFrame();
        TrustFrame(const LedgerEntry& from);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new TrustFrame(*this)); }

        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);

		//TxResultCode fromTx(AccountEntry& signingAccount, TrustSetTx* tx);
		
        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
		
	};
}

#endif
