#ifndef __LEDGER__
#define __LEDGER__


#include "generated/stellar.hh"


namespace stellar
{
	/*
		Ledger headers + ?
	*/

	class Ledger
	{
	public:

        stellarxdr::uint256     mHash;
        stellarxdr::uint256     mParentHash;
        stellarxdr::uint256     mTransHash;
        stellarxdr::uint256     mAccountHash;
		std::uint64_t      mTotCoins;
		std::uint64_t      mFeePool;		// All the fees collected since last inflation spend
		std::uint32_t		mLedgerSeq;
		std::uint32_t		mInflationSeq;	// the last inflation that was applied 
		
		std::uint64_t      mCloseTime;         // when this ledger closed (seconds since 1970)
		std::uint64_t      mParentCloseTime;   // when the previous ledger closed
		
		
		bool        mClosed, mValidated, mValidHash, mAccepted, mImmutable;

		std::uint32_t      mReferenceFeeUnits;                 // Fee units for the reference transaction
		std::uint32_t      mReserveBase, mReserveIncrement;    // Reserve base and increment in fee units
		std::uint64_t      mBaseFee;                           // Stroop cost of the reference transaction


		typedef std::shared_ptr<Ledger>           pointer;

		Ledger();

		std::uint64_t scaleFeeBase(std::uint64_t fee);
		std::uint64_t getReserve(int increments);

		void updateFees();
	};
}

#endif