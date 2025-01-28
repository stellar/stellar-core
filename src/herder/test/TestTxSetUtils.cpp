// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerManager.h"
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
makeGeneralizedTxSetXDR(std::vector<PhaseComponents> const& phases,
                        Hash const& previousLedgerHash,
                        bool useParallelSorobanPhase)
{
    GeneralizedTransactionSet xdrTxSet(1);
    for (size_t i = 0; i < phases.size(); ++i)
    {
        releaseAssert(i < static_cast<size_t>(TxSetPhase::PHASE_COUNT));
        auto const& phase = phases[i];
        bool isParallelSorobanPhase = false;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        if (useParallelSorobanPhase &&
            i == static_cast<size_t>(TxSetPhase::SOROBAN))
        {
            releaseAssert(phase.size() <= 1);
            isParallelSorobanPhase = true;
        }
#endif

        auto normalizedTxsPerBaseFee = phase;
        std::sort(normalizedTxsPerBaseFee.begin(),
                  normalizedTxsPerBaseFee.end());
        for (auto& [_, txs] : normalizedTxsPerBaseFee)
        {
            txs = TxSetUtils::sortTxsInHashOrder(txs);
        }

        xdrTxSet.v1TxSet().previousLedgerHash = previousLedgerHash;
        auto& xdrPhase = xdrTxSet.v1TxSet().phases.emplace_back();
        if (isParallelSorobanPhase)
        {
            xdrPhase.v(1);
        }
        for (auto const& [baseFee, txs] : normalizedTxsPerBaseFee)
        {
            if (isParallelSorobanPhase)
            {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                auto& component = xdrPhase.parallelTxsComponent();
                if (baseFee)
                {
                    component.baseFee.activate() = *baseFee;
                }
                if (!txs.empty())
                {
                    auto& cluster =
                        component.executionStages.emplace_back().emplace_back();
                    for (auto const& tx : txs)
                    {
                        cluster.emplace_back(tx->getEnvelope());
                    }
                }
#else
                releaseAssert(false);
#endif
            }
            else
            {
                auto& component = xdrPhase.v0Components().emplace_back(
                    TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
                if (baseFee)
                {
                    component.txsMaybeDiscountedFee().baseFee.activate() =
                        *baseFee;
                }
                auto& componentTxs = component.txsMaybeDiscountedFee().txs;
                for (auto const& tx : txs)
                {
                    componentTxs.emplace_back(tx->getEnvelope());
                }
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
    std::vector<PhaseComponents> const& txsPerBaseFee, Application& app,
    Hash const& previousLedgerHash, std::optional<bool> useParallelSorobanPhase)
{
    if (!useParallelSorobanPhase.has_value())
    {
        useParallelSorobanPhase =
            protocolVersionStartsFrom(app.getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header.ledgerVersion,
                                      PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    }

    auto xdrTxSet = makeGeneralizedTxSetXDR(txsPerBaseFee, previousLedgerHash,
                                            *useParallelSorobanPhase);
    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    return std::make_pair(txSet, txSet->prepareForApply(app));
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedTxSetBasedOnLedgerVersion(
    std::vector<TransactionFrameBasePtr> const& txs, Application& app,
    Hash const& previousLedgerHash)
{
    if (protocolVersionStartsFrom(app.getLedgerManager()
                                      .getLastClosedLedgerHeader()
                                      .header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        return makeNonValidatedGeneralizedTxSet(
            {{std::make_pair(100LL, txs)}, {}}, app, previousLedgerHash);
    }
    else
    {
        return makeNonValidatedTxSet(txs, app, previousLedgerHash);
    }
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void
normalizeParallelPhaseXDR(TransactionPhase& phase)
{
    auto compareTxHash = [](TransactionEnvelope const& tx1,
                            TransactionEnvelope const& tx2) -> bool {
        return xdrSha256(tx1) < xdrSha256(tx2);
    };
    for (auto& stage : phase.parallelTxsComponent().executionStages)
    {
        for (auto& cluster : stage)
        {
            std::sort(cluster.begin(), cluster.end(), compareTxHash);
        }
        std::sort(stage.begin(), stage.end(),
                  [&](auto const& c1, auto const& c2) {
                      return compareTxHash(c1.front(), c2.front());
                  });
    }
    std::sort(phase.parallelTxsComponent().executionStages.begin(),
              phase.parallelTxsComponent().executionStages.end(),
              [&](auto const& s1, auto const& s2) {
                  return compareTxHash(s1.front().front(), s2.front().front());
              });
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedGeneralizedTxSet(PhaseComponents const& classicTxsPerBaseFee,
                                 std::optional<int64_t> sorobanBaseFee,
                                 TxStageFrameList const& sorobanTxsPerStage,
                                 Application& app,
                                 Hash const& previousLedgerHash)
{
    auto xdrTxSet = makeGeneralizedTxSetXDR({classicTxsPerBaseFee},
                                            previousLedgerHash, false);
    xdrTxSet.v1TxSet().phases.emplace_back(1);
    auto& phase = xdrTxSet.v1TxSet().phases.back();
    if (sorobanBaseFee)
    {
        phase.parallelTxsComponent().baseFee.activate() = *sorobanBaseFee;
    }

    auto& stages = phase.parallelTxsComponent().executionStages;
    for (auto const& stage : sorobanTxsPerStage)
    {
        auto& xdrStage = stages.emplace_back();
        for (auto const& cluster : stage)
        {
            auto& xdrCluster = xdrStage.emplace_back();
            for (auto const& tx : cluster)
            {
                xdrCluster.emplace_back(tx->getEnvelope());
            }
        }
    }
    normalizeParallelPhaseXDR(phase);
    auto txSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    return std::make_pair(txSet, txSet->prepareForApply(app));
}
#endif

} // namespace testtxset
} // namespace stellar
