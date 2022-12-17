// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/simulation/TxSimTxSetFrame.h"
#include "transactions/simulation/TxSimFeeBumpTransactionFrame.h"
#include "transactions/simulation/TxSimTransactionFrame.h"

namespace stellar
{
namespace txsimulation
{

SimApplyOrderTxSetFrame::SimApplyOrderTxSetFrame(
    LedgerHeaderHistoryEntry const& lclHeader,
    Transactions const& txsInApplyOrder)
    : TxSetFrame(lclHeader, txsInApplyOrder), mTxsInApplyOrder(txsInApplyOrder)
{
    computeTxFeesForNonGeneralizedSet(lclHeader.header);
    computeContentsHash();
}

TxSetFrameConstPtr
makeSimTxSetFrame(Hash const& networkID,
                  LedgerHeaderHistoryEntry const& lclHeader,
                  std::vector<TransactionEnvelope> const& transactions,
                  std::vector<TransactionResultPair> const& results,
                  uint32_t multiplier)
{
    std::vector<TransactionFrameBasePtr> txs;
    txs.reserve(transactions.size());

    auto resultIter = results.cbegin();
    uint32_t partition = 0;
    for (auto const& txEnv : transactions)
    {
        TransactionFrameBasePtr txFrame;
        switch (txEnv.type())
        {
        case ENVELOPE_TYPE_TX_V0:
        case ENVELOPE_TYPE_TX:
            txFrame = std::make_shared<TxSimTransactionFrame>(
                networkID, txEnv, resultIter->result, partition);
            break;
        case ENVELOPE_TYPE_TX_FEE_BUMP:
            txFrame = std::make_shared<TxSimFeeBumpTransactionFrame>(
                networkID, txEnv, resultIter->result, partition);
            break;
        default:
            abort();
        }

        txs.emplace_back(txFrame);
        ++resultIter;
        if (++partition == multiplier)
        {
            partition = 0;
        }
    }

    assert(resultIter == results.end());
    return std::make_shared<SimApplyOrderTxSetFrame const>(lclHeader, txs);
}

}
}
