#include "fba/Ballot.h"
#include "ledger/Ledger.h"
#include "generated/stellar.hh"

/*
    LATER: How do we use the excluded ballots in preparing?
*/



namespace stellar
{

    Ballot::Ballot(LedgerPtr ledger, stellarxdr::uint256 const& txSetHash, uint64_t closeTime)
    {
        mIndex = 1;
        mLedgerCloseTime = closeTime;
        mTxSetHash = txSetHash;
        mLederIndex = ledger->mLedgerSeq + 1;
        mPreviousLedgerHash = ledger->mHash;
    }

    Ballot::Ballot(Ballot::pointer other)
    {
        mIndex = other->mIndex;
        mLedgerCloseTime = other->mLedgerCloseTime;
        mTxSetHash = other->mTxSetHash;
        mLederIndex = other->mLederIndex;
        mPreviousLedgerHash = other->mPreviousLedgerHash;
    }

    Ballot::Ballot(stellarxdr::SlotBallot xdrballot)
    {
        mIndex = xdrballot.ballot.index;
        mLedgerCloseTime = xdrballot.ballot.closeTime;
        mTxSetHash = xdrballot.ballot.txSetHash;
        mLederIndex = xdrballot.ledgerIndex;
        mPreviousLedgerHash = xdrballot.previousLedgerHash;
    }


    Ballot::Ballot(stellarxdr::Ballot xdrballot)
    {
        mIndex = xdrballot.index;
        mLedgerCloseTime = xdrballot.closeTime;
        mTxSetHash = xdrballot.txSetHash;
    }

	bool Ballot::isCompatible(Ballot::pointer other)
	{
		if(!other) return false;
		if(mTxSetHash != other->mTxSetHash) return false;
		if(mLedgerCloseTime != other->mLedgerCloseTime) return false;

		return true;
	}

	bool Ballot::compareValue(Ballot::pointer other)
	{
		if(mTxSetHash > other->mTxSetHash) return true;
		if(mLedgerCloseTime > other->mLedgerCloseTime) return true;

		return false;
	}

	// returns true if this is higher
	// returns false if it is lower or we don't have one of the txsets or they are the same
	bool Ballot::compare(Ballot::pointer other)
	{
		if(!other) return true;

		if(mIndex > other->mIndex) return true;
		if(mIndex < other->mIndex) return false;
		return compareValue(other);
	}

    void Ballot::toXDR(stellarxdr::SlotBallot& slotBallot)
    {
        slotBallot.ledgerIndex = mLederIndex;
        slotBallot.previousLedgerHash = mPreviousLedgerHash;
        toXDR(slotBallot.ballot);
    }
    void Ballot::toXDR(stellarxdr::Ballot& ballot)
    {
        ballot.closeTime = mLedgerCloseTime;
        ballot.index = mIndex;
        ballot.txSetHash = mTxSetHash;
    }
}

/*
// returns true if this is higher
// returns false if it is lower or we don't have one of the txsets or they are the same
bool Ballot::compare(Ballot::pointer other)
{
if(!other) return true;

if(mIndex > other->mIndex) return true;
if(mIndex < other->mIndex) return false;

if(!mValue || !(other->mValue)) return false;
if(mValue == other->mValue) return(false);
return(mValue >= other->mValue);

if(mTxSet->size() > other->mTxSet->size()) return true;
if(mTxSet->size() < other->mTxSet->size()) return false;
if(mTxSet->getHash() > other->mTxSet->getHash()) return true;
return false;
}
*/
