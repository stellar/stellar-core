#include "Transaction.h"

namespace stellar
{
	Transaction::pointer Transaction::makeTransactionFromWire(stellarxdr::TransactionEnvelope& msg)
	{
		// SANITY check sig
        return Transaction::pointer();
	}

    bool Transaction::isValid()
    {
        // SANITY
        return true;
    }

    stellarxdr::uint256 Transaction::getHash()
	{
		if(isZero(mHash))
		{
			// serialize and then hash
            // SANITY
		}
		return(mHash);
	}

    TxResultCode Transaction::apply()
	{
		return(doApply());
	}

    void Transaction::toXDR(stellarxdr::TransactionEnvelope& envelope)
    {
        // SANITY
    }
}
