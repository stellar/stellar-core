#ifndef __LEDGERENTRY__
#define __LEDGERENTRY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "Database.h"
#include "lib/json/json-forwards.h"

/*
LedgerEntry
Parent of AccountEntry, TrustLine, OfferEntry
*/
namespace stellar
{
    class LedgerMaster;

	class LedgerEntry
	{
	protected:
        
		stellarxdr::uint256 mIndex;

		virtual void calculateIndex() = 0;
	public:
		typedef std::shared_ptr<LedgerEntry> pointer;

        stellarxdr::LedgerEntry mEntry;

        LedgerEntry(const stellarxdr::LedgerEntry& from);


        virtual LedgerEntry::pointer copy() const=0;

		// calculate the index if you don't have it already
        stellarxdr::uint256 getIndex();

		// calculate the hash if you don't have it already
        stellarxdr::uint256 getHash();

		
		// these will do the appropriate thing in the DB and the json txResult
		virtual void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster)=0;
		virtual void storeChange(LedgerEntry::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster)=0;
		virtual void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster)=0;

        static void dropAll(Database &db); // deletes all data from DB
	};
}

#endif
