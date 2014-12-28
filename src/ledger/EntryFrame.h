#ifndef __ENTRYFRAME__
#define __ENTRYFRAME__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "lib/json/json-forwards.h"

/*
Frame
Parent of AccountFrame, TrustFrame, OfferFrame

These just hold the xdr LedgerEntry objects and have some associated functions
*/

namespace stellar
{
    class LedgerMaster;
    class Database;
    /*
    extern void getIndex(const LedgerEntry& entry, const uint256& retIndex);

    extern void storeDelete(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster);
    extern void storeChange(const LedgerEntry& entry, const LedgerEntry& startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster);
    extern void storeAdd(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster);
    */
    
	class EntryFrame
	{
	protected:
        
		uint256 mIndex;

		virtual void calculateIndex() = 0;
	public:
		typedef std::shared_ptr<EntryFrame> pointer;

        LedgerEntry mEntry;

        EntryFrame();
        EntryFrame(const LedgerEntry& from);
        

        virtual EntryFrame::pointer copy() const=0;

		// calculate the index if you don't have it already
        uint256 getIndex();

		// calculate the hash if you don't have it already
        uint256 getHash();

		
		// these will do the appropriate thing in the DB and the json txResult
		virtual void storeDelete(Json::Value& txResult, LedgerMaster& ledgerMaster)=0;
		virtual void storeChange(EntryFrame::pointer startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster)=0;
		virtual void storeAdd(Json::Value& txResult, LedgerMaster& ledgerMaster)=0;

        static void dropAll(Database &db); // deletes all data from DB
	};
}

#endif
