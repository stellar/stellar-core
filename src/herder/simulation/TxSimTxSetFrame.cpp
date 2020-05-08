// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/simulation/TxSimTxSetFrame.h"
#include "crypto/SHA.h"
#include "transactions/TransactionBridge.h"
#include "transactions/simulation/TxSimFeeBumpTransactionFrame.h"
#include "transactions/simulation/TxSimTransactionFrame.h"
#include "xdrpp/marshal.h"
#include <numeric>

namespace stellar
{
namespace txsimulation
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

TxSimTxSetFrame::TxSimTxSetFrame(
    Hash const& networkID, Hash const& previousLedgerHash,
    std::vector<TransactionEnvelope> const& transactions,
    std::vector<TransactionResultPair> const& results, uint32_t multiplier)
    : mNetworkID(networkID)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTransactions(transactions)
    , mResults(results)
    , mContentsHash(
          computeContentsHash(mNetworkID, mPreviousLedgerHash, mTransactions))
    , mMultiplier(multiplier)
{
}

int64_t
TxSimTxSetFrame::getBaseFee(LedgerHeader const& lh) const
{
    return 0;
}

Hash const&
TxSimTxSetFrame::getContentsHash()
{
    return mContentsHash;
}

Hash const&
TxSimTxSetFrame::previousLedgerHash() const
{
    return mPreviousLedgerHash;
}

size_t
TxSimTxSetFrame::sizeTx() const
{
    return mTransactions.size();
}

size_t
TxSimTxSetFrame::sizeOp() const
{
    return std::accumulate(mTransactions.begin(), mTransactions.end(),
                           size_t(0), [](size_t a, TransactionEnvelope txEnv) {
                               auto ops = txbridge::getOperations(txEnv).size();
                               if (txEnv.type() == ENVELOPE_TYPE_TX_FEE_BUMP)
                               {
                                   ++ops;
                               }
                               return a + ops;
                           });
}

std::vector<TransactionFrameBasePtr>
TxSimTxSetFrame::sortForApply()
{
    std::vector<TransactionFrameBasePtr> res;
    res.reserve(mTransactions.size());

    auto resultIter = mResults.cbegin();
    uint32_t partition = 0;
    for (auto const& txEnv : mTransactions)
    {
        TransactionFrameBasePtr txFrame;
        switch (txEnv.type())
        {
        case ENVELOPE_TYPE_TX_V0:
        case ENVELOPE_TYPE_TX:
            txFrame = std::make_shared<TxSimTransactionFrame>(
                mNetworkID, txEnv, resultIter->result, partition);
            break;
        case ENVELOPE_TYPE_TX_FEE_BUMP:
            txFrame = std::make_shared<TxSimFeeBumpTransactionFrame>(
                mNetworkID, txEnv, resultIter->result, partition);
            break;
        default:
            abort();
        }

        res.emplace_back(txFrame);
        ++resultIter;
        if (++partition == mMultiplier)
        {
            partition = 0;
        }
    }

    assert(resultIter == mResults.end());
    return res;
}

void
TxSimTxSetFrame::toXDR(TransactionSet& set)
{
    // Delegate to TxSetFrame and explicitly call sortForHash on it for now;
    // likely this whole class will go away at some point.
    TransactionSet txSet;
    txSet.previousLedgerHash = mPreviousLedgerHash;
    txSet.txs.insert(txSet.txs.end(), mTransactions.begin(),
                     mTransactions.end());
    TxSetFrame tf(mNetworkID, txSet);
    tf.sortForHash();
    tf.toXDR(set);
}
}
}
