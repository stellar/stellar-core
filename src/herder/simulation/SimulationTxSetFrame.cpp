// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/simulation/SimulationTxSetFrame.h"
#include "crypto/SHA.h"
#include "transactions/simulation/SimulationTransactionFrame.h"
#include "xdrpp/marshal.h"
#include <numeric>

namespace stellar
{

static Hash
computeContentsHash(Hash const& networkID, Hash const& previousLedgerHash,
                    std::vector<TransactionEnvelope> transactions)
{
    TransactionSet txSet;
    txSet.previousLedgerHash = previousLedgerHash;
    txSet.txs.insert(txSet.txs.end(), transactions.begin(), transactions.end());
    return TxSetFrame(networkID, txSet).getContentsHash();
}

SimulationTxSetFrame::SimulationTxSetFrame(
    Hash const& networkID, Hash const& previousLedgerHash,
    std::vector<TransactionEnvelope> const& transactions,
    std::vector<TransactionResultPair> const& results)
    : mNetworkID(networkID)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTransactions(transactions)
    , mResults(results)
    , mContentsHash(
          computeContentsHash(mNetworkID, mPreviousLedgerHash, mTransactions))
{
}

int64_t
SimulationTxSetFrame::getBaseFee(LedgerHeader const& lh) const
{
    return 0;
}

Hash const&
SimulationTxSetFrame::getContentsHash()
{
    return mContentsHash;
}

Hash const&
SimulationTxSetFrame::previousLedgerHash() const
{
    return mPreviousLedgerHash;
}

size_t
SimulationTxSetFrame::sizeTx() const
{
    return mTransactions.size();
}

size_t
SimulationTxSetFrame::sizeOp() const
{
    return std::accumulate(mTransactions.begin(), mTransactions.end(),
                           size_t(0),
                           [](size_t a, TransactionEnvelope const& txEnv) {
                               return a + txEnv.tx.operations.size();
                           });
}

std::vector<TransactionFramePtr>
SimulationTxSetFrame::sortForApply()
{
    std::vector<TransactionFramePtr> res;
    res.reserve(mTransactions.size());

    auto resultIter = mResults.cbegin();
    for (auto const& txEnv : mTransactions)
    {
        res.emplace_back(SimulationTransactionFrame::makeTransactionFromWire(
            mNetworkID, txEnv, resultIter->result));
        ++resultIter;
    }
    return res;
}
}
