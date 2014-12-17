#ifndef __LEDGERENTRY__
#define __LEDGERENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "LedgerDatabase.h"

/*
LedgerEntry
Parent of AccountEntry, TrustLine, OfferEntry
*/
namespace stellar
{
	class LedgerEntry
	{
	protected:
		stellarxdr::uint256 mIndex;

		virtual void insertIntoDB() = 0;
		virtual void updateInDB() = 0;
		virtual void deleteFromDB() = 0;

		virtual void calculateIndex() = 0;
	public:
		typedef std::shared_ptr<LedgerEntry> pointer;

		// calculate the index if you don't have it already
        stellarxdr::uint256 getIndex();

		// calculate the hash if you don't have it already
        stellarxdr::uint256 getHash();

		
		// these will do the appropriate thing in the DB and the preimage
		void storeDelete();
		void storeChange();
		void storeAdd();

        static void dropAll(LedgerDatabase &db); // deletes all data from DB
	};
}

#endif
