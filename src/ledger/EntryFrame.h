#ifndef __ENTRYFRAME__
#define __ENTRYFRAME__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "lib/json/json.h"

/*
Frame
Parent of AccountFrame, TrustFrame, OfferFrame

These just hold the xdr LedgerEntry objects and have some associated functions
*/

namespace stellar
{
    class LedgerMaster;
    class Database;
    
    
	class EntryFrame
	{
	protected:
        
        bool mValid;
		uint256 mIndex;

		virtual void calculateIndex() = 0;
	public:
		typedef std::shared_ptr<EntryFrame> pointer;

        LedgerEntry mEntry;

        EntryFrame();
        EntryFrame(const LedgerEntry& from);
        

        virtual EntryFrame::pointer copy() const=0;

        bool isValid() { return mValid;  }
		// calculate the index if you don't have it already
        uint256 getIndex();

		// calculate the hash if you don't have it already
        uint256 getHash();

		
		// these will do the appropriate thing in the DB and the json txResult
		virtual void storeDelete(rapidjson::Value& txResult, LedgerMaster& ledgerMaster)=0;
		virtual void storeChange(EntryFrame::pointer startFrom, rapidjson::Value& txResult, LedgerMaster& ledgerMaster)=0;
		virtual void storeAdd(rapidjson::Value& txResult, LedgerMaster& ledgerMaster)=0;

        static void dropAll(Database &db); // deletes all data from DB
	};
}

#endif
