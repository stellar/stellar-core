// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
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

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedTxSet(std::vector<TransactionFrameBasePtr> const& txs,
                      Application& app, Hash const& previousLedgerHash)
{
    auto xdrTxSet = makeTxSetXDR(txs, previousLedgerHash);
    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    auto applicableTxSet = txSet->prepareForApply(app);
    return std::make_pair(txSet, std::move(applicableTxSet));
}
} // namespace

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedGeneralizedTxSet(
    std::vector<ComponentPhases> const& txsPerBaseFee, Application& app,
    Hash const& previousLedgerHash)
{
    auto xdrTxSet = makeGeneralizedTxSetXDR(txsPerBaseFee, previousLedgerHash);
    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    return std::make_pair(txSet, txSet->prepareForApply(app));
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedTxSetBasedOnLedgerVersion(
    uint32_t ledgerVersion, std::vector<TransactionFrameBasePtr> const& txs,
    Application& app, Hash const& previousLedgerHash)
{
    if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        return makeNonValidatedGeneralizedTxSet(
            {{std::make_pair(100LL, txs)}, {}}, app, previousLedgerHash);
    }
    else
    {
        return makeNonValidatedTxSet(txs, app, previousLedgerHash);
    }
}

} // namespace testtxset
} // namespace stellar
