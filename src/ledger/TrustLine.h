#ifndef __TRUSTLINE__
#define __TRUSTLINE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/LedgerEntry.h"
#include "transactions/TxResultCode.h"

namespace stellar {

	class TrustSetTx;
	class AccountEntry;

	//index is: 
	class TrustLine : public LedgerEntry
	{
		void calculateIndex();
		
	public:
        typedef std::shared_ptr<TrustLine> pointer;

		TrustLine(const stellarxdr::LedgerEntry& from);

        LedgerEntry::pointer copy()  const { return LedgerEntry::pointer(new TrustLine(*this)); }

        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(LedgerEntry::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);

		TxResultCode fromTx(AccountEntry& signingAccount, TrustSetTx* tx);
		
        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
		
	};
}

#endif
