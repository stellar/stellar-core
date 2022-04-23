// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestTxSetUtils.h"

namespace stellar
{

TxSetFrameConstPtr
TestTxSetUtils::addTxs(TxSetFrameConstPtr txSet,
                       TxSetFrame::Transactions const& newTxs)
{
    auto updated = txSet->getTxsInHashOrder();
    updated.insert(updated.end(), newTxs.begin(), newTxs.end());
    return std::make_shared<TxSetFrame const>(txSet->previousLedgerHash(),
                                              updated);
}

TxSetFrameConstPtr
TestTxSetUtils::removeTxs(TxSetFrameConstPtr txSet,
                          TxSetFrame::Transactions const& txsToRemove)
{
    return TxSetUtils::removeTxs(txSet, txsToRemove);
}

TxSetFrameConstPtr
TestTxSetUtils::makeIllSortedTxSet(Hash const& networkID,
                                   TxSetFrameConstPtr goodTxSet)
{
    TransactionSet xdrSet;
    goodTxSet->toXDR(xdrSet);
    // rearrange it out of order
    releaseAssert(xdrSet.txs.size() >= 2);
    std::swap(xdrSet.txs[0], xdrSet.txs[1]);
    return std::make_shared<TxSetFrame const>(networkID, xdrSet);
}

} // namespace stellar