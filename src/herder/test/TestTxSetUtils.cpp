// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestTxSetUtils.h"

#include <map>

namespace stellar
{
TxSetFrameConstPtr
TestTxSetUtils::makeNonValidatedTxSet(
    std::vector<TransactionFrameBasePtr> const& txs, Hash const& networkID,
    Hash const& previousLedgerHash)
{
    auto xdrTxSet = TestTxSetUtils::makeTxSetXDR(txs, previousLedgerHash);
    return TxSetFrame::makeFromWire(networkID, xdrTxSet);
}

TransactionSet
TestTxSetUtils::makeTxSetXDR(std::vector<TransactionFrameBasePtr> const& txs,
                             Hash const& previousLedgerHash)
{
    auto normalizedTxs = TxSetUtils::sortTxsInHashOrder(txs);
    TransactionSet txSet;
    txSet.previousLedgerHash = previousLedgerHash;
    for (auto const& tx : txs)
    {
        txSet.txs.push_back(tx->getEnvelope());
    }
    return txSet;
}

GeneralizedTransactionSet
TestTxSetUtils::makeGeneralizedTxSetXDR(
    std::vector<std::pair<std::optional<int64_t>,
                          std::vector<TransactionFrameBasePtr>>> const&
        txsPerBaseFee,
    Hash const& previousLedgerHash)
{
    auto normalizedTxsPerBaseFee = txsPerBaseFee;
    std::sort(normalizedTxsPerBaseFee.begin(), normalizedTxsPerBaseFee.end());
    for (auto& [_, txs] : normalizedTxsPerBaseFee)
    {
        txs = TxSetUtils::sortTxsInHashOrder(txs);
    }

    GeneralizedTransactionSet xdrTxSet(1);
    xdrTxSet.v1TxSet().previousLedgerHash = previousLedgerHash;
    auto& phase = xdrTxSet.v1TxSet().phases.emplace_back();
    for (auto const& [baseFee, txs] : normalizedTxsPerBaseFee)
    {
        auto& component = phase.v0Components().emplace_back(
            TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
        if (baseFee)
        {
            component.txsMaybeDiscountedFee().baseFee.activate() = *baseFee;
        }
        auto& componentTxs = component.txsMaybeDiscountedFee().txs;
        for (auto const& tx : txs)
        {
            componentTxs.emplace_back(tx->getEnvelope());
        }
    }
    return xdrTxSet;
}
// TxSetFrameConstPtr
// TestTxSetUtils::addTxs(TxSetFrameConstPtr txSet,
//                        TxSetFrame::Transactions const& newTxs)
//{
//     //auto updated = txSet->getTxsInHashOrder();
//     //updated.insert(updated.end(), newTxs.begin(), newTxs.end());
//     //return TxSetFrame::makeFromTransactions(txSet->previousLedgerHash(),
//     //                                        updated);
// }
//
// TxSetFrameConstPtr
// TestTxSetUtils::removeTxs(TxSetFrameConstPtr txSet,
//                           TxSetFrame::Transactions const& txsToRemove)
//{
//     //return TxSetUtils::removeTxs(txSet, txsToRemove);
// }
//
// TxSetFrameConstPtr
// TestTxSetUtils::makeIllSortedTxSet(Hash const& networkID,
//                                    TxSetFrameConstPtr goodTxSet)
//{
//     //TransactionSet xdrSet;
//     //goodTxSet->toXDR(xdrSet);
//     //// rearrange it out of order
//     //releaseAssert(xdrSet.txs.size() >= 2);
//     //std::swap(xdrSet.txs[0], xdrSet.txs[1]);
//     //return std::make_shared<TxSetFrame const>(networkID, xdrSet);
// }

} // namespace stellar