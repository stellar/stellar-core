#ifndef __OFFERENTRY__
#define __OFFERENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LedgerEntry.h"


namespace stellar
{
	class OfferEntry : public LedgerEntry
	{
		void calculateIndex();
	public:
	
 		OfferEntry(const stellarxdr::LedgerEntry& from);
		//OfferEntry(SLE::pointer sle);

        LedgerEntry::pointer copy()  const { return LedgerEntry::pointer(new OfferEntry(*this)); }

        void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeChange(LedgerEntry::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
        void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster);

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}

#endif
