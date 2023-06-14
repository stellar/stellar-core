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
makeGeneralizedTxSetXDR(std::vector<ComponentPhases> const& txsPerBaseFeePhases,
                        Hash const& previousLedgerHash)
{
    if (txsPerBaseFeePhases.size() !=
        static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT))
    {
        throw std::runtime_error(
            "makeGeneralizedTxSetXDR: invalid number of phases");
    }
    GeneralizedTransactionSet xdrTxSet(1);
    for (auto& txsPerBaseFee : txsPerBaseFeePhases)
    {
        auto normalizedTxsPerBaseFee = txsPerBaseFee;
        std::sort(normalizedTxsPerBaseFee.begin(),
                  normalizedTxsPerBaseFee.end());
        for (auto& [_, txs] : normalizedTxsPerBaseFee)
        {
            txs = TxSetUtils::sortTxsInHashOrder(txs);
        }

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
    }
    return xdrTxSet;
}

TxSetFrameConstPtr
makeNonValidatedTxSet(std::vector<TransactionFrameBasePtr> const& txs,
                      Application& app, Hash const& previousLedgerHash)
{
    auto xdrTxSet = makeTxSetXDR(txs, previousLedgerHash);
    return TxSetFrame::makeFromWire(app, xdrTxSet);
}
} // namespace

TxSetFrameConstPtr
makeNonValidatedGeneralizedTxSet(
    std::vector<ComponentPhases> const& txsPerBaseFee, Application& app,
    Hash const& previousLedgerHash)
{
    if (txsPerBaseFee.size() >
        static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT))
    {
        throw std::runtime_error("makeNonValidatedGeneralizedTxSet: invalid "
                                 "parameter, too many phases");
    }
    // Potentially add any empty phases to make the tx set valid
    auto normalizedTxsPerBaseFee = txsPerBaseFee;
    normalizedTxsPerBaseFee.resize(
        static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT));
    auto xdrTxSet =
        makeGeneralizedTxSetXDR(normalizedTxsPerBaseFee, previousLedgerHash);
    return TxSetFrame::makeFromWire(app, xdrTxSet);
}

TxSetFrameConstPtr
makeNonValidatedTxSetBasedOnLedgerVersion(
    uint32_t ledgerVersion, std::vector<TransactionFrameBasePtr> const& txs,
    Application& app, Hash const& previousLedgerHash)
{
    if (protocolVersionStartsFrom(ledgerVersion,
                                  GENERALIZED_TX_SET_PROTOCOL_VERSION))
    {
        return makeNonValidatedGeneralizedTxSet({{std::make_pair(100LL, txs)}},
                                                app, previousLedgerHash);
    }
    else
    {
        return makeNonValidatedTxSet(txs, app, previousLedgerHash);
    }
}

} // namespace testtxset
} // namespace stellar