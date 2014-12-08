#ifndef __BUCKETLIST__
#define __BUCKETLIST__

#include <map>
#include "generated/stellar.hh"
#include "CanonicalLedgerForm.h"


/*
bucket list will be a collection of temporal buckets
It stores the hashes of the ledger entries


*/
namespace stellar
{

	class BucketList : public CanonicalLedgerForm
	{
        
		// index , LedgerEntry
		std::map<stellarxdr::uint256, LedgerEntry::pointer> mPendingAdds;
		std::map<stellarxdr::uint256, LedgerEntry::pointer> mPendingUpdates;
		std::map<stellarxdr::uint256, LedgerEntry::pointer> mPendingDeletes;
        stellarxdr::uint256 mHash;
		void calculateHash();
	public:

		bool load(stellarxdr::uint256 ledgerHash);

        ////////////////////////////////
        /// from CLFGateway
        void recvDelta(CLFDeltaPtr delta);
        LedgerHeaderPtr getCurrentHeader();


		//void setParentBucketList(BucketList::pointer parent);
		void addEntry(stellarxdr::uint256& newHash, LedgerEntry::pointer newEntry);
		void updateEntry(stellarxdr::uint256& oldHash, stellarxdr::uint256& newHash, LedgerEntry::pointer updatedEntry);
		void deleteEntry(stellarxdr::uint256& hash);
		void closeLedger();  // need to call after all the tx have been applied to save that last versions of the ledger entries into the buckets

        stellarxdr::uint256 getHash();
    };
}
#endif