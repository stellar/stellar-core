#include "TransactionSet.h"

namespace stellar
{
    TransactionSet::TransactionSet()
    {

    }
    TransactionSet::TransactionSet(stellarxdr::TransactionSet& xdrSet)
    {
        // SANITY
    }

	void TransactionSet::serialize()
	{
        // SANITY
	}

	/*
	bool TransactionSet::operator > (const TransactionSet& other)
	{
		if(mTransactions.size() > other.mTransactions.size()) return true;
		if(mTransactions.size() < other.mTransactions.size()) return false;
		if(getContentsHash() > other.getContentsHash()) return true;
		return false;
	}
	*/


	Transaction::pointer TransactionSet::getTransaction(stellarxdr::uint256& txHash)
	{
		for each (auto tx in mTransactions)
		{
			if(txHash == tx->getHash()) return(tx);
		}
		return(Transaction::pointer());
	}

	// save this tx set to the node store in serialized format
	void TransactionSet::store()
	{
		// SANITY
	}

    void TransactionSet::toXDR(stellarxdr::TransactionSet& txSet)
    {
        txSet.txs.resize(mTransactions.size());
        for(int n = 0; n < mTransactions.size(); n++)
        {
            mTransactions[n]->toXDR(txSet.txs[n]);
        }
    }
}