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
		void insertIntoDB();
		void updateInDB();
		void deleteFromDB();

		void calculateIndex();
	public:
		


 		OfferEntry();
		//OfferEntry(SLE::pointer sle);

        static void dropAll(LedgerDatabase &db);
        static const char *kSQLCreateStatement;
	};
}

#endif
