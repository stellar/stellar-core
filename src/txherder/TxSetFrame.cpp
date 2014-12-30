// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TxSetFrame.h"
#include "xdrpp/marshal.h"

namespace stellar
{
    TxSetFrame::TxSetFrame()
{
}

TxSetFrame::TxSetFrame(TransactionSet const& xdrSet)
{
    for (auto txEnvelope : xdrSet.txs)
    {
        TransactionFramePtr tx =
            TransactionFrame::makeTransactionFromWire(txEnvelope);
        mTransactions.push_back(tx);
    }
}

bool TxSetFrame::checkValid(Application& app)
{
    for(auto tx : mTransactions)
    {
        if(!tx->checkValid(app)) return false;
    }
    return true;
}

uint256
TxSetFrame::getContentsHash()
{
    if (isZero(mHash))
    {
        TransactionSet txSet;
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

TransactionFramePtr
TxSetFrame::getTransaction(uint256 const& txHash)
{
    for (auto tx : mTransactions)
    {
        if (txHash == tx->getHash())
            return (tx);
    }
    return (TransactionFramePtr());
}

// save this tx set to the node store in serialized format
void
TxSetFrame::store()
{
    // LATER
}

void
TxSetFrame::toXDR(TransactionSet& txSet)
{
    txSet.txs.resize(mTransactions.size());
    for (unsigned int n = 0; n < mTransactions.size(); n++)
    {
        mTransactions[n]->toXDR(txSet.txs[n].tx);
        txSet.txs[n].signature = mTransactions[n]->getSignature();
    }
}
}
