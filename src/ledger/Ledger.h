#ifndef __LEDGER__
#define __LEDGER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"

namespace stellar
{
	/*
		Ledger headers + some stuff cached for txs to use during application?
	*/

	class Ledger
	{
        
	public:
        stellarxdr::LedgerHeader mHeader;

        
		std::uint64_t      mParentCloseTime;   // when the previous ledger closed
		
		
		bool        mClosed, mValidated, mValidHash, mAccepted, mImmutable;

		std::uint32_t      mReferenceFeeUnits;                 // Fee units for the reference transaction
		std::uint32_t      mReserveBase, mReserveIncrement;    // Reserve base and increment in fee units
		


		typedef std::shared_ptr<Ledger>           pointer;

		Ledger();

        uint64_t getMinBalance(); 
        uint64_t getTxFee();

		std::uint64_t scaleFeeBase(std::uint64_t fee);
		std::uint64_t getReserve(int increments);

		void updateFees();

        void applyTx(const stellarxdr::Transaction& tx);
        void isTxValid(const stellarxdr::Transaction& tx);
	};
}

#endif
