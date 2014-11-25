#ifndef __BUCKETLIST__
#define __BUCKETLIST__

#include <map>
#include "generated/stellar.hh"
#include "CanonicalLedgerForm.h"


/*
bucket list will be a collection of temporal buckets
It stores the hashes of the ledger entries

Do we need the SLE in the bucket list?

*/
namespace stellar
{

	class BucketList : public CanonicalLedgerForm
	{
        /* LATER 
		// index , SLE
		std::map<stellarxdr::uint256, SLE::pointer> mPendingAdds;
		std::map<stellarxdr::uint256, SLE::pointer> mPendingUpdates;
		std::map<stellarxdr::uint256, SLE::pointer> mPendingDeletes;
		uint256 mHash;
		void calculateHash();
	public:

		bool load(stellarxdr::uint256 ledgerHash);

		//void setParentBucketList(BucketList::pointer parent);
		void addEntry(stellarxdr::uint256& newHash, SLE::pointer newEntry);
		void updateEntry(stellarxdr::uint256& oldHash, stellarxdr::uint256& newHash, SLE::pointer updatedEntry);
		void deleteEntry(stellarxdr::uint256& hash);
		void closeLedger();  // need to call after all the tx have been applied to save that last versions of the ledger entries into the buckets

		uint256 getHash();
        */
	};
}
#endif