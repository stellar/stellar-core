#include "Transaction.h"
#include "xdrpp/marshal.h"

namespace stellar
{
	Transaction::pointer Transaction::makeTransactionFromWire(stellarxdr::TransactionEnvelope& msg)
	{
        //mSignature = msg.signature;

		// SANITY check sig
        return Transaction::pointer();
	}

    // check for the malformed, correct sig, min fee
    bool Transaction::isValid()
    {
        // NICOLAS
        return true;
    }

    stellarxdr::uint256 Transaction::getSignature()
    {
        return mSignature;
    }

    stellarxdr::uint256 Transaction::getHash()
	{
		if(isZero(mHash))
		{
            stellarxdr::Transaction tx;
            toXDR(tx);
            xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(tx));
            hashXDR(std::move(xdrBytes), mHash);
		}
		return(mHash);
	}

    TxResultCode Transaction::apply()
	{
		return(doApply());
	}

    void Transaction::toXDR(stellarxdr::Transaction& envelope)
    {
        // LATER
    }

    void Transaction::toXDR(stellarxdr::TransactionEnvelope& envelope)
    {
        // LATER
    }

    StellarMessagePtr Transaction::toStellarMessage()
    {
        StellarMessagePtr msg = std::make_shared<stellarxdr::StellarMessage>();
        msg->type(stellarxdr::TRANSACTION);
        toXDR(msg->transaction());
        return msg;
    }
}
