// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Ledger.h"
#include "main/Application.h"
#include "lib/json/json.h"

namespace stellar
{
    Ledger::Ledger()
    {

    }

    // TODO.2
    int64_t Ledger::getMinBalance()
    {
        return 0;
    }
    int64_t Ledger::getTxFee()
    {
        return 0;
    }

    /* NICOLAS
	std::uint64_t Ledger::getReserve(int increments)
	{
		if(!mBaseFee) updateFees();

		return scaleFeeBase(static_cast<std::uint64_t> (increments)* mReserveIncrement + mReserveBase);
	}

	std::uint64_t Ledger::scaleFeeBase(std::uint64_t fee)
	{
		if(!mBaseFee)
			updateFees();

		return getApp().getFeeTrack().scaleFeeBase(fee, mBaseFee, mReferenceFeeUnits);
	}


	void Ledger::updateFees()
	{
		std::uint64_t baseFee = getConfig().FEE_DEFAULT;
		std::uint32_t referenceFeeUnits = 10;
		std::uint32_t reserveBase = getConfig().FEE_ACCOUNT_RESERVE;
		std::int64_t reserveIncrement = getConfig().FEE_OWNER_RESERVE;

		// NICOLAS
		LedgerStateParms p = lepNONE;
		SLE::pointer sle = getASNode(p, Ledger::getLedgerFeeIndex(), ltFEE_SETTINGS);

		if(sle)
		{
			if(sle->getFieldIndex(sfBaseFee) != -1)
				baseFee = sle->getFieldU64(sfBaseFee);

			if(sle->getFieldIndex(sfReferenceFeeUnits) != -1)
				referenceFeeUnits = sle->getFieldU32(sfReferenceFeeUnits);

			if(sle->getFieldIndex(sfReserveBase) != -1)
				reserveBase = sle->getFieldU32(sfReserveBase);

			if(sle->getFieldIndex(sfReserveIncrement) != -1)
				reserveIncrement = sle->getFieldU32(sfReserveIncrement);
		}

		{
			StaticScopedLockType sl(sPendingSaveLock);
			if(mBaseFee == 0)
			{
				mBaseFee = baseFee;
				mReferenceFeeUnits = referenceFeeUnits;
				mReserveBase = reserveBase;
				mReserveIncrement = reserveIncrement;
			}
		}
		
	}
    */
}
