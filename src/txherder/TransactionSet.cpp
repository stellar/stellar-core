#include "TransactionSet.h"
#include "xdrpp/marshal.h"

namespace stellar
{
TransactionSet::TransactionSet()
{
}

TransactionSet::TransactionSet(stellarxdr::TransactionSet &xdrSet)
{
    for (auto txEnvelope : xdrSet.txs)
    {
        Transaction::pointer tx =
            Transaction::makeTransactionFromWire(txEnvelope);
        mTransactions.push_back(tx);
    }
}

stellarxdr::uint256
TransactionSet::getContentsHash()
{
    if (isZero(mHash))
    {
        stellarxdr::TransactionSet txSet;
        toXDR(txSet);
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(txSet));
        hashXDR(std::move(xdrBytes), mHash);
    }
    return mHash;
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

Transaction::pointer
TransactionSet::getTransaction(stellarxdr::uint256 &txHash)
{
    for (auto tx : mTransactions)
    {
        if (txHash == tx->getHash())
            return (tx);
    }
    return (Transaction::pointer());
}

// save this tx set to the node store in serialized format
void
TransactionSet::store()
{
    // LATER
}

void
TransactionSet::toXDR(stellarxdr::TransactionSet &txSet)
{
    txSet.txs.resize(mTransactions.size());
    for (unsigned int n = 0; n < mTransactions.size(); n++)
    {
        mTransactions[n]->toXDR(txSet.txs[n].tx);
        txSet.txs[n].signature = mTransactions[n]->getSignature();
    }
}
}
