#ifndef __BALLOT__
#define __BALLOT__

#include "txherder/TransactionSet.h"
#include "fba/Node.h"
#include "generated/stellar.hh"

namespace stellar
{
	class Ledger;
	typedef std::shared_ptr<Ledger> LedgerPtr;

	// <n,x>
	class Ballot
	{

	public:
		typedef std::shared_ptr<Ballot> pointer;

		uint32_t mLederIndex;					// the slot
        stellarxdr::uint256 mPreviousLedgerHash;		// the slot

		int mIndex;							// n
		uint64_t mLedgerCloseTime;			// x
        stellarxdr::uint256 mTxSetHash;					// x
		//BallotValue::pointer mValue;

        Ballot(stellarxdr::Ballot xdrballot);
		Ballot(Ballot::pointer other);
        Ballot(stellarxdr::SlotBallot xdrballot);
		Ballot(LedgerPtr ledger, stellarxdr::uint256& txSetHash, uint64_t closeTime);
		

		// returns true if this ballot is ranked higher
		bool compare(Ballot::pointer other);
		bool compareValue(Ballot::pointer other);
		bool isCompatible(Ballot::pointer other);

        void toXDR(stellarxdr::SlotBallot& slotBallot);
        void toXDR(stellarxdr::Ballot& ballot);
	};
}

#endif