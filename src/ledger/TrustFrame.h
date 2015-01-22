#ifndef __TRUSTLINE__
#define __TRUSTLINE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"

namespace stellar {

	class TrustSetTx;
	
	class TrustFrame : public EntryFrame
	{
		void calculateIndex();
		
	public:
        typedef std::shared_ptr<TrustFrame> pointer;

        TrustFrame();
        TrustFrame(const LedgerEntry& from);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new TrustFrame(*this)); }

        void storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster);
        void storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster);
        void storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster);

        int64_t getBalance();

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}

#endif
