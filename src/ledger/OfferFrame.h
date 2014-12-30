#ifndef __OFFERENTRY__
#define __OFFERENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"


namespace stellar
{
	class OfferFrame : public EntryFrame
	{
		void calculateIndex();
	public:

        OfferFrame();
        OfferFrame(const LedgerEntry& from);
        void from(const Transaction& tx);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new OfferFrame(*this)); }

        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}

#endif
