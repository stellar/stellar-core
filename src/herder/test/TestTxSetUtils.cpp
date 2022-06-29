// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/test/TestTxSetUtils.h"
#include "util/ProtocolVersion.h"

#include <map>

namespace stellar
{
namespace testtxset
{
namespace
{
TransactionSet
makeTxSetXDR(std::vector<TransactionFrameBasePtr> const& txs,
             Hash const& previousLedgerHash)
{
    TransactionSet txSet;
    txSet.previousLedgerHash = previousLedgerHash;
    auto normalizedTxs = TxSetUtils::sortTxsInHashOrder(txs);
    for (auto const& tx : normalizedTxs)
    {
        txSet.txs.push_back(tx->getEnvelope());
    }
    return txSet;
}

GeneralizedTransactionSet
makeGeneralizedTxSetXDR(
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

TxSetFrameConstPtr
makeNonValidatedTxSet(std::vector<TransactionFrameBasePtr> const& txs,
                      Hash const& networkID, Hash const& previousLedgerHash)
{
    auto xdrTxSet = makeTxSetXDR(txs, previousLedgerHash);
    return TxSetFrame::makeFromWire(networkID, xdrTxSet);
}
} // namespace

TxSetFrameConstPtr
makeNonValidatedGeneralizedTxSet(
    std::vector<std::pair<std::optional<int64_t>,
                          std::vector<TransactionFrameBasePtr>>> const&
        txsPerBaseFee,
    Hash const& networkID, Hash const& previousLedgerHash)
{
    auto xdrTxSet = makeGeneralizedTxSetXDR(txsPerBaseFee, previousLedgerHash);
    return TxSetFrame::makeFromWire(networkID, xdrTxSet);
}

TxSetFrameConstPtr
makeNonValidatedTxSetBasedOnLedgerVersion(
    uint32_t ledgerVersion, std::vector<TransactionFrameBasePtr> const& txs,
    Hash const& networkID, Hash const& previousLedgerHash)
{
    if (protocolVersionStartsFrom(ledgerVersion,
                                  GENERALIZED_TX_SET_PROTOCOL_VERSION))
    {
        return makeNonValidatedGeneralizedTxSet({std::make_pair(100LL, txs)},
                                                networkID, previousLedgerHash);
    }
    else
    {
        return makeNonValidatedTxSet(txs, networkID, previousLedgerHash);
    }
}

} // namespace testtxset
} // namespace stellar