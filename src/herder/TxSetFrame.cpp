// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetFrame.h"
#include "TxSetUtils.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/HerderImpl.h"
#include "herder/ParallelTxSetBuilder.h"
#include "herder/SurgePricingUtils.h"
#include "ledger/ImmutableLedgerView.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "util/numeric.h"
#include "xdrpp/marshal.h"
#ifdef BUILD_TESTS
#include "test/Catch2.h"
#endif

#include <Tracy.hpp>
#include <algorithm>
#include <future>
#include <limits>
#include <list>
#include <numeric>
#include <variant>

namespace stellar
{

namespace
{
std::string
getTxSetPhaseName(TxSetPhase phase)
{
    switch (phase)
    {
    case TxSetPhase::CLASSIC:
        return "classic";
    case TxSetPhase::SOROBAN:
        return "soroban";
    default:
        throw std::runtime_error("Unknown phase");
    }
}

bool
validateSequentialPhaseXDRStructure(TransactionPhase const& phase)
{
    bool componentsNormalized =
        std::is_sorted(phase.v0Components().begin(), phase.v0Components().end(),
                       [](auto const& c1, auto const& c2) {
                           if (!c1.txsMaybeDiscountedFee().baseFee ||
                               !c2.txsMaybeDiscountedFee().baseFee)
                           {
                               return !c1.txsMaybeDiscountedFee().baseFee &&
                                      c2.txsMaybeDiscountedFee().baseFee;
                           }
                           return *c1.txsMaybeDiscountedFee().baseFee <
                                  *c2.txsMaybeDiscountedFee().baseFee;
                       });
    if (!componentsNormalized)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: incorrect component order");
        return false;
    }

    bool componentBaseFeesUnique =
        std::adjacent_find(phase.v0Components().begin(),
                           phase.v0Components().end(),
                           [](auto const& c1, auto const& c2) {
                               if (!c1.txsMaybeDiscountedFee().baseFee ||
                                   !c2.txsMaybeDiscountedFee().baseFee)
                               {
                                   return !c1.txsMaybeDiscountedFee().baseFee &&
                                          !c2.txsMaybeDiscountedFee().baseFee;
                               }
                               return *c1.txsMaybeDiscountedFee().baseFee ==
                                      *c2.txsMaybeDiscountedFee().baseFee;
                           }) == phase.v0Components().end();
    if (!componentBaseFeesUnique)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: duplicate component base fees");
        return false;
    }
    for (auto const& component : phase.v0Components())
    {
        if (component.txsMaybeDiscountedFee().txs.empty())
        {
            CLOG_DEBUG(Herder, "Got bad txSet: empty component");
            return false;
        }
    }
    return true;
}

bool
validateParallelComponent(ParallelTxsComponent const& component,
                          uint32_t ledgerVersion)
{
    // A base fee without any transactions to apply it to has no effect, so
    // base fee should be 0, (see
    // CANONICAL_EMPTY_SOROBAN_PHASE_PROTOCOL_VERSION) comment in
    // ProtocolVersion.h for more details).
    if (component.executionStages.empty() && component.baseFee &&
        protocolVersionStartsFrom(
            ledgerVersion, CANONICAL_EMPTY_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        CLOG_DEBUG(Herder, "Got bad txSet: base fee without stages");
        return false;
    }
    for (auto const& stage : component.executionStages)
    {
        if (stage.empty())
        {
            CLOG_DEBUG(Herder, "Got bad txSet: empty stage");
            return false;
        }
        for (auto const& cluster : stage)
        {
            if (cluster.empty())
            {
                CLOG_DEBUG(Herder, "Got bad txSet: empty cluster");
                return false;
            }
        }
    }
    return true;
}

bool
validateTxSetXDRStructure(GeneralizedTransactionSet const& txSet,
                          uint32_t ledgerVersion)
{
    int const MAX_PHASE = 1;

    if (txSet.v() != 1)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: unsupported version {}", txSet.v());
        return false;
    }
    auto phaseCount = static_cast<size_t>(TxSetPhase::PHASE_COUNT);
    auto const& txSetV1 = txSet.v1TxSet();
    // There was no protocol with 1 phase, so checking for 2 phases only
    if (txSetV1.phases.size() != phaseCount)
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet: exactly 2 phases are expected, got {}",
                   txSetV1.phases.size());
        return false;
    }

    for (size_t phaseId = 0; phaseId < phaseCount; ++phaseId)
    {
        auto const& phase = txSetV1.phases[phaseId];
        if (phase.v() > MAX_PHASE)
        {
            CLOG_DEBUG(Herder, "Got bad txSet: unsupported phase version {}",
                       phase.v());
            return false;
        }
        if (phase.v() == 1)
        {
            if (phaseId != static_cast<size_t>(TxSetPhase::SOROBAN))
            {
                CLOG_DEBUG(Herder,
                           "Got bad txSet: non-Soroban parallel phase {}",
                           phase.v());
                return false;
            }
            if (!validateParallelComponent(phase.parallelTxsComponent(),
                                           ledgerVersion))
            {
                return false;
            }
        }
        else
        {
            if (!validateSequentialPhaseXDRStructure(phase))
            {
                return false;
            }
        }
    }
    return true;
}

// We want to XOR the tx hash with the set hash.
// This way people can't predict the order that txs will be applied in
struct ApplyTxSorter
{
    Hash mSetHash;
    ApplyTxSorter(Hash h) : mSetHash{std::move(h)}
    {
    }

    bool
    operator()(TransactionFrameBasePtr const& tx1,
               TransactionFrameBasePtr const& tx2) const
    {
        // need to use the hash of whole tx here since multiple txs could
        // have the same Contents
        return lessThanXored(tx1->getFullHash(), tx2->getFullHash(), mSetHash);
    }
};

Hash
computeNonGeneralizedTxSetContentsHash(TransactionSet const& xdrTxSet)
{
    ZoneScoped;
    SHA256 hasher;
    hasher.add(xdrTxSet.previousLedgerHash);
    for (auto const& tx : xdrTxSet.txs)
    {
        hasher.add(xdr::xdr_to_opaque(tx));
    }
    return hasher.finish();
}

// Note: Soroban txs also use this functionality for simplicity, as it's a
// no-op (all Soroban txs have 1 op max)
int64_t
computePerOpFee(TransactionFrameBase const& tx, uint32_t ledgerVersion)
{
    auto rounding =
        protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION)
            ? Rounding::ROUND_DOWN
            : Rounding::ROUND_UP;
    auto txOps = std::max(tx.getNumOperations(), 1u);
    return bigDivideOrThrow(tx.getInclusionFee(), 1,
                            static_cast<int64_t>(txOps), rounding);
}

void
transactionsToTransactionSetXDR(TxFrameList const& txs,
                                Hash const& previousLedgerHash,
                                TransactionSet& txSet)
{
    ZoneScoped;
    txSet.txs.resize(xdr::size32(txs.size()));
    auto sortedTxs = TxSetUtils::sortTxsInHashOrder(txs);
    for (unsigned int n = 0; n < sortedTxs.size(); n++)
    {
        txSet.txs[n] = sortedTxs[n]->getEnvelope();
    }
    txSet.previousLedgerHash = previousLedgerHash;
}

void
sequentialPhaseToXdr(TxFrameList const& txs,
                     InclusionFeeMap const& inclusionFeeMap,
                     TransactionPhase& xdrPhase)
{
    xdrPhase.v(0);

    std::map<std::optional<int64_t>, size_t> feeTxCount;
    for (auto const& [_, fee] : inclusionFeeMap)
    {
        ++feeTxCount[fee];
    }
    auto& components = xdrPhase.v0Components();
    // Reserve a component per unique base fee in order to have the correct
    // pointers in componentPerBid map.
    components.reserve(feeTxCount.size());

    std::map<std::optional<int64_t>, xdr::xvector<TransactionEnvelope>*>
        componentPerBid;
    for (auto const& [fee, txCount] : feeTxCount)
    {
        components.emplace_back(TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
        auto& discountedFeeComponent =
            components.back().txsMaybeDiscountedFee();
        if (fee)
        {
            discountedFeeComponent.baseFee.activate() = *fee;
        }
        componentPerBid[fee] = &discountedFeeComponent.txs;
        componentPerBid[fee]->reserve(txCount);
    }
    auto sortedTxs = TxSetUtils::sortTxsInHashOrder(txs);
    for (auto const& tx : sortedTxs)
    {
        componentPerBid[inclusionFeeMap.find(tx)->second]->push_back(
            tx->getEnvelope());
    }
}

void
parallelPhaseToXdr(TxStageFrameList const& txs,
                   InclusionFeeMap const& inclusionFeeMap,
                   TransactionPhase& xdrPhase)
{
    xdrPhase.v(1);

    std::optional<int64_t> baseFee;
    if (!inclusionFeeMap.empty())
    {
        baseFee = inclusionFeeMap.begin()->second;
    }
    // We currently don't support multi-component parallel perPhaseTxs, so make
    // sure all txs have the same base fee.
    for (auto const& [_, fee] : inclusionFeeMap)
    {
        releaseAssert(fee == baseFee);
    }
    auto& component = xdrPhase.parallelTxsComponent();
    if (baseFee)
    {
        component.baseFee.activate() = *baseFee;
    }
    component.executionStages.reserve(txs.size());
    auto sortedTxs = TxSetUtils::sortParallelTxsInHashOrder(txs);
    for (auto const& stage : sortedTxs)
    {
        auto& xdrStage = component.executionStages.emplace_back();
        xdrStage.reserve(stage.size());
        for (auto const& cluster : stage)
        {
            auto& xdrCluster = xdrStage.emplace_back();
            xdrCluster.reserve(cluster.size());
            for (auto const& tx : cluster)
            {
                xdrCluster.push_back(tx->getEnvelope());
            }
        }
    }
}

void
transactionsToGeneralizedTransactionSetXDR(
    std::vector<TxSetPhaseFrame> const& phases, Hash const& previousLedgerHash,
    GeneralizedTransactionSet& generalizedTxSet)
{
    ZoneScoped;

    generalizedTxSet.v(1);
    generalizedTxSet.v1TxSet().previousLedgerHash = previousLedgerHash;
    generalizedTxSet.v1TxSet().phases.resize(phases.size());
    for (int i = 0; i < phases.size(); ++i)
    {
        auto const& txPhase = phases[i];
        txPhase.toXDR(generalizedTxSet.v1TxSet().phases[i]);
    }
}

TxFrameList
sortedForApplySequential(TxFrameList const& txs, Hash const& txSetHash)
{
    TxFrameList retList;
    retList.reserve(txs.size());

    auto txQueues = TxSetUtils::buildAccountTxQueues(txs);

    // build txBatches
    // txBatches i-th element contains each i-th transaction for
    // accounts with a transaction in the transaction set
    std::vector<std::vector<TransactionFrameBasePtr>> txBatches;

    while (!txQueues.empty())
    {
        txBatches.emplace_back();
        auto& curBatch = txBatches.back();
        // go over all users that still have transactions
        for (auto it = txQueues.begin(); it != txQueues.end();)
        {
            auto& txQueue = *it;
            curBatch.emplace_back(txQueue->getTopTx());
            txQueue->popTopTx();
            if (txQueue->empty())
            {
                // done with that user
                it = txQueues.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& batch : txBatches)
    {
        // randomize each batch using the hash of the transaction set
        // as a way to randomize even more
        ApplyTxSorter s(txSetHash);
        std::sort(batch.begin(), batch.end(), s);
        for (auto const& tx : batch)
        {
            retList.push_back(tx);
        }
    }

    return retList;
}

TxStageFrameList
sortedForApplyParallel(TxStageFrameList const& stages, Hash const& txSetHash)
{
    ZoneScoped;
    TxStageFrameList sortedStages = stages;
    ApplyTxSorter sorter(txSetHash);
    for (auto& stage : sortedStages)
    {
        for (auto& cluster : stage)
        {
            std::sort(cluster.begin(), cluster.end(), sorter);
        }
        // There is no need to shuffle clusters in the stage, as they are
        // independent, so the apply order doesn't matter even if the clusters
        // are being applied sequentially.
    }
    std::sort(sortedStages.begin(), sortedStages.end(),
              [&sorter](auto const& a, auto const& b) {
                  releaseAssert(!a.empty() && !b.empty());
                  releaseAssert(!a.front().empty() && !b.front().empty());
                  return sorter(a.front().front(), b.front().front());
              });
    return sortedStages;
}

bool
addWireTxsToList(Hash const& networkID,
                 xdr::xvector<TransactionEnvelope> const& xdrTxs,
                 TxFrameList& txList)
{
    auto prevSize = txList.size();
    txList.reserve(prevSize + xdrTxs.size());
    for (auto const& env : xdrTxs)
    {
        auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, env);
        if (!tx->XDRProvidesValidFee() || tx->getInclusionFee() <= 0)
        {
            return false;
        }
        txList.push_back(tx);
    }
    if (!std::is_sorted(txList.begin() + prevSize, txList.end(),
                        &TxSetUtils::hashTxSorter))
    {
        return false;
    }
    return true;
}

std::vector<int64_t>
computeLaneBaseFee(TxSetPhase phase, LedgerHeader const& ledgerHeader,
                   SurgePricingLaneConfig const& surgePricingConfig,
                   std::vector<int64_t> const& lowestLaneFee,
                   std::vector<bool> const& hadTxNotFittingLane)
{
    std::vector<int64_t> laneBaseFee(lowestLaneFee.size(),
                                     ledgerHeader.baseFee);
    auto minBaseFee =
        *std::min_element(lowestLaneFee.begin(), lowestLaneFee.end());
    for (size_t lane = 0; lane < laneBaseFee.size(); ++lane)
    {
        // If generic lane is full, then any transaction had to compete with not
        // included transactions and independently of the lane they need to have
        // at least the minimum fee in the tx set applied.
        if (hadTxNotFittingLane[SurgePricingPriorityQueue::GENERIC_LANE])
        {
            laneBaseFee[lane] = minBaseFee;
        }
        // If limited lane is full, then the transactions in this lane also had
        // to compete with each other and have a base fee associated with this
        // lane only.
        if (lane != SurgePricingPriorityQueue::GENERIC_LANE &&
            hadTxNotFittingLane[lane])
        {
            laneBaseFee[lane] = lowestLaneFee[lane];
        }
        if (laneBaseFee[lane] > ledgerHeader.baseFee)
        {
            CLOG_WARNING(
                Herder,
                "{} phase: surge pricing for '{}' lane is in effect with base "
                "fee={}, baseFee={}",
                getTxSetPhaseName(phase),
                lane == SurgePricingPriorityQueue::GENERIC_LANE ? "generic"
                                                                : "DEX",
                laneBaseFee[lane], ledgerHeader.baseFee);
        }
    }
    return laneBaseFee;
}

std::shared_ptr<SurgePricingLaneConfig>
createSurgePricingLaneConfig(TxSetPhase phase, Config const& cfg,
                             TxSetBuildLimits const& limits)
{
    ZoneScoped;

    auto const& lclHeader = limits.mLclHeader;
    std::vector<bool> hadTxNotFittingLane;
    std::shared_ptr<SurgePricingLaneConfig> surgePricingLaneConfig;
    if (phase == TxSetPhase::CLASSIC)
    {
        auto maxOps =
            Resource({limits.maxTxSetSizeOps(),
                      static_cast<uint32_t>(cfg.getClassicByteAllowance())});
        std::optional<Resource> dexOpsLimit;
        if (cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET)
        {
            // DEX operations limit implies that DEX transactions should
            // compete with each other in in a separate fee lane, which
            // is only possible with generalized tx set.
            dexOpsLimit = Resource({*cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET,
                                    MAX_CLASSIC_BYTE_ALLOWANCE});
        }

        surgePricingLaneConfig =
            std::make_shared<DexLimitingLaneConfig>(maxOps, dexOpsLimit);
    }
    else
    {
        releaseAssert(phase == TxSetPhase::SOROBAN);
        releaseAssert(limits.mSorobanConfig);

        auto limitsRes = limits.mSorobanConfig->maxLedgerResources();
        // When building Soroban tx sets with parallel execution support,
        // instructions are accounted for by the build logic, not by the surge
        // pricing config, so we need to relax the instruction limit in surge
        // pricing logic.
        if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                      PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
        {
            limitsRes.setVal(Resource::Type::INSTRUCTIONS,
                             std::numeric_limits<int64_t>::max());
        }

        auto byteLimit =
            std::min(static_cast<int64_t>(cfg.getSorobanByteAllowance()),
                     limitsRes.getVal(Resource::Type::TX_BYTE_SIZE));
        limitsRes.setVal(Resource::Type::TX_BYTE_SIZE, byteLimit);

        surgePricingLaneConfig =
            std::make_shared<SorobanGenericLaneConfig>(limitsRes);
    }
    return surgePricingLaneConfig;
}

TxFrameList
buildSurgePricedSequentialPhase(
    TxFrameList const& txs,
    std::shared_ptr<SurgePricingLaneConfig> surgePricingLaneConfig,
    std::vector<bool>& hadTxNotFittingLane, uint32_t ledgerVersion,
    std::optional<size_t> seed)
{
    ZoneScoped;
    return SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
        txs, surgePricingLaneConfig, hadTxNotFittingLane, ledgerVersion, seed);
}

// The base fee every transaction of a selected phase is included at: per
// surge pricing lane, the ledger's base fee, or the lowest fee rate included
// if transactions of the lane (or of the generic lane) were left out.
std::shared_ptr<InclusionFeeMap>
computeInclusionFeeMap(
    TxSetPhase phase,
    std::variant<TxFrameList, TxStageFrameList> const& includedTxs,
    SurgePricingLaneConfig const& surgePricingLaneConfig,
    LedgerHeader const& lclHeader, std::vector<bool> const& hadTxNotFittingLane)
{
    auto visitIncludedTxs =
        [&includedTxs](
            std::function<void(TransactionFrameBaseConstPtr const&)> visitor) {
            std::visit(
                [&visitor](auto const& txs) {
                    using T = std::decay_t<decltype(txs)>;
                    if constexpr (std::is_same_v<T, TxFrameList>)
                    {
                        for (auto const& tx : txs)
                        {
                            visitor(tx);
                        }
                    }
                    else if constexpr (std::is_same_v<T, TxStageFrameList>)
                    {
                        for (auto const& stage : txs)
                        {
                            for (auto const& thread : stage)
                            {
                                for (auto const& tx : thread)
                                {
                                    visitor(tx);
                                }
                            }
                        }
                    }
                    else
                    {
                        // This can't be just `false` as if an assertion is not
                        // dependent on template argument, it will be
                        // unconditionally triggered.
                        static_assert(!std::is_same_v<T, T>,
                                      "Non-exhaustive visitor");
                    }
                },
                includedTxs);
        };

    std::vector<int64_t> lowestLaneFee;
    size_t laneCount = surgePricingLaneConfig.getLaneLimits().size();
    releaseAssert(hadTxNotFittingLane.size() == laneCount);
    lowestLaneFee.resize(laneCount, std::numeric_limits<int64_t>::max());
    visitIncludedTxs(
        [&lowestLaneFee, &surgePricingLaneConfig, &lclHeader](auto const& tx) {
            size_t lane = surgePricingLaneConfig.getLane(*tx);
            auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
            lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
        });
    auto laneBaseFee =
        computeLaneBaseFee(phase, lclHeader, surgePricingLaneConfig,
                           lowestLaneFee, hadTxNotFittingLane);
    auto inclusionFeeMapPtr = std::make_shared<InclusionFeeMap>();
    auto& inclusionFeeMap = *inclusionFeeMapPtr;
    visitIncludedTxs([&inclusionFeeMap, &laneBaseFee,
                      &surgePricingLaneConfig](auto const& tx) {
        inclusionFeeMap[tx] = laneBaseFee[surgePricingLaneConfig.getLane(*tx)];
    });
    return inclusionFeeMapPtr;
}

// `hadTxNotFittingLaneOut`, if given, receives per surge pricing lane of the
// phase whether a transaction was left out for the lane's limits.
std::pair<std::variant<TxFrameList, TxStageFrameList>,
          std::shared_ptr<InclusionFeeMap>>
applySurgePricing(TxSetPhase phase, TxFrameList const& txs, Config const& cfg,
                  TxSetBuildLimits const& limits, std::optional<size_t> seed,
                  std::vector<bool>* hadTxNotFittingLaneOut
#ifdef BUILD_TESTS
                  ,
                  bool enforceTxsApplyOrder,
                  txtest::ParallelSorobanOrder const& parallelSorobanOrder
#endif
)
{
    ZoneScoped;
    auto surgePricingLaneConfig =
        createSurgePricingLaneConfig(phase, cfg, limits);
    std::vector<bool> hadTxNotFittingLane;
    uint32_t ledgerVersion = limits.mLclHeader.ledgerVersion;
    bool isParallelSoroban =
        phase == TxSetPhase::SOROBAN &&
        protocolVersionStartsFrom(ledgerVersion,
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    std::variant<TxFrameList, TxStageFrameList> includedTxs;
    if (isParallelSoroban)
    {
#ifdef BUILD_TESTS
        if (enforceTxsApplyOrder)
        {
            TxStageFrameList frameList;
            if (!txs.empty())
            {
                for (auto const& stageIndexes : parallelSorobanOrder)
                {
                    TxStageFrame stage;
                    for (auto const& threadIndexes : stageIndexes)
                    {
                        TxFrameList threadTxs;
                        for (auto const& txIndex : threadIndexes)
                        {
                            threadTxs.push_back(txs.at(txIndex));
                        }
                        stage.emplace_back(std::move(threadTxs));
                    }
                    frameList.emplace_back(std::move(stage));
                }

                // If the order is empty, we default to a single
                // thread with all transactions in it.
                if (parallelSorobanOrder.empty())
                {
                    frameList = {{txs}};
                }
            }
            includedTxs = frameList;

            // soroban only has one fee lane
            hadTxNotFittingLane.emplace_back(false);
        }
        else
        {
#endif
            releaseAssert(limits.mSorobanConfig);
            includedTxs = buildSurgePricedParallelSorobanPhase(
                txs, cfg, *limits.mSorobanConfig, surgePricingLaneConfig,
                hadTxNotFittingLane, ledgerVersion, seed);
#ifdef BUILD_TESTS
        }
#endif
    }
    else
    {
        includedTxs = buildSurgePricedSequentialPhase(
            txs, surgePricingLaneConfig, hadTxNotFittingLane, ledgerVersion,
            seed);
    }

    auto inclusionFeeMapPtr =
        computeInclusionFeeMap(phase, includedTxs, *surgePricingLaneConfig,
                               limits.mLclHeader, hadTxNotFittingLane);
    if (hadTxNotFittingLaneOut)
    {
        *hadTxNotFittingLaneOut = hadTxNotFittingLane;
    }
    return std::make_pair(includedTxs, inclusionFeeMapPtr);
}

size_t
countOps(TxFrameList const& txs)
{
    return std::accumulate(txs.begin(), txs.end(), size_t(0),
                           [&](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

int64_t
computeBaseFeeForLegacyTxSet(LedgerHeader const& lclHeader,
                             TxFrameList const& txs)
{
    ZoneScoped;
    auto ledgerVersion = lclHeader.ledgerVersion;
    int64_t lowestBaseFee = std::numeric_limits<int64_t>::max();
    for (auto const& tx : txs)
    {
        int64_t txBaseFee = computePerOpFee(*tx, ledgerVersion);
        lowestBaseFee = std::min(lowestBaseFee, txBaseFee);
    }
    int64_t baseFee = lclHeader.baseFee;

    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_11))
    {
        size_t surgeOpsCutoff = 0;
        if (lclHeader.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lclHeader.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (countOps(txs) > surgeOpsCutoff)
        {
            baseFee = lowestBaseFee;
        }
    }
    return baseFee;
}

TxSetValidationResult
checkFeeMap(InclusionFeeMap const& feeMap, LedgerHeader const& lclHeader)
{
    for (auto const& [tx, fee] : feeMap)
    {
        if (!fee)
        {
            continue;
        }
        if (*fee < lclHeader.baseFee)
        {

            CLOG_DEBUG(Herder,
                       "Got bad txSet: {} has too low component "
                       "base fee {}",
                       hexAbbrev(lclHeader.previousLedgerHash), *fee);
            return TxSetValidationResult::COMPONENT_BASE_FEE_TOO_LOW;
        }
        if (tx->getInclusionFee() < getMinInclusionFee(*tx, lclHeader, fee))
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: {} has tx with fee bid ({}) lower "
                       "than base fee ({})",
                       hexAbbrev(lclHeader.previousLedgerHash),
                       tx->getInclusionFee(),
                       getMinInclusionFee(*tx, lclHeader, fee));
            return TxSetValidationResult::TX_FEE_BID_TOO_LOW;
        }
    }
    return TxSetValidationResult::VALID;
}

} // namespace

TxSetXDRFrame::TxSetXDRFrame(TransactionSet const& xdrTxSet)
    : mXDRTxSet(xdrTxSet)
    , mEncodedSize(xdr::xdr_argpack_size(xdrTxSet))
    , mHash(computeNonGeneralizedTxSetContentsHash(xdrTxSet))
{
}

TxSetXDRFrame::TxSetXDRFrame(GeneralizedTransactionSet const& xdrTxSet)
    : mXDRTxSet(xdrTxSet)
    , mEncodedSize(xdr::xdr_argpack_size(xdrTxSet))
    , mHash(xdrSha256(xdrTxSet))
{
}

TxSetXDRFrame::TxSetXDRFrame(TransactionSet&& xdrTxSet)
    : mXDRTxSet(std::move(xdrTxSet))
    , mEncodedSize(xdr::xdr_argpack_size(std::get<TransactionSet>(mXDRTxSet)))
    , mHash(computeNonGeneralizedTxSetContentsHash(
          std::get<TransactionSet>(mXDRTxSet)))
{
}

TxSetXDRFrame::TxSetXDRFrame(GeneralizedTransactionSet&& xdrTxSet)
    : mXDRTxSet(std::move(xdrTxSet))
    , mEncodedSize(
          xdr::xdr_argpack_size(std::get<GeneralizedTransactionSet>(mXDRTxSet)))
    , mHash(xdrSha256(std::get<GeneralizedTransactionSet>(mXDRTxSet)))
{
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromWire(TransactionSet&& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetXDRFrame> txSet(
        new TxSetXDRFrame(std::move(xdrTxSet)));
    return txSet;
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromWire(GeneralizedTransactionSet&& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetXDRFrame> txSet(
        new TxSetXDRFrame(std::move(xdrTxSet)));
    return txSet;
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromWire(TransactionSet const& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetXDRFrame> txSet(new TxSetXDRFrame(xdrTxSet));
    return txSet;
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromWire(GeneralizedTransactionSet const& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetXDRFrame> txSet(new TxSetXDRFrame(xdrTxSet));
    return txSet;
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromStoredTxSet(StoredTransactionSet const& storedSet)
{
    if (storedSet.v() == 0)
    {
        return TxSetXDRFrame::makeFromWire(storedSet.txSet());
    }
    return TxSetXDRFrame::makeFromWire(storedSet.generalizedTxSet());
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    PerPhaseTransactionList const& txPhases, Application& app,
    ApplyTimeOffset closeTimeOffset
#ifdef BUILD_TESTS
    ,
    bool enforceTxsApplyOrder,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder,
    bool disableTxValidationForLegacyScenario
#endif
)
{
    PerPhaseTransactionList invalidTxs;
    invalidTxs.resize(txPhases.size());
    return makeTxSetFromTransactions(txPhases, app, closeTimeOffset, invalidTxs
#ifdef BUILD_TESTS
                                     ,
                                     enforceTxsApplyOrder, parallelSorobanOrder,
                                     disableTxValidationForLegacyScenario
#endif
    );
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    PerPhaseTransactionList const& txPhases, Application& app,
    ApplyTimeOffset closeTimeOffset, PerPhaseTransactionList& invalidTxs
#ifdef BUILD_TESTS
    ,
    bool enforceTxsApplyOrder,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder,
    bool disableTxValidationForLegacyScenario
#endif
    ,
    TxSetReadyCallback const& onTxSetReady,
    std::vector<size_t> const& validTxTargets,
    TxValidationCache* validationCache,
    std::shared_ptr<TxSetSelfCheck const>* deferredSelfCheck)
{
    releaseAssert(threadIsMain());
    releaseAssert(!app.getLedgerManager().isApplying());
    releaseAssert(txPhases.size() == invalidTxs.size());

    // Everything is read from the LCL: validity through the context, the
    // limits of the set from its header and Soroban configuration.
    auto ctx = TxSetValidationContext::forLCL(app);
    ctx.mValidationCache = validationCache;
    auto const validatedPhases = TxSetBuilder::selectPhases(
        txPhases, app, TxSetBuildLimits::forLCL(app), ctx, closeTimeOffset,
        invalidTxs, validTxTargets, "build", std::nullopt, nullptr
#ifdef BUILD_TESTS
        ,
        enforceTxsApplyOrder, parallelSorobanOrder,
        disableTxValidationForLegacyScenario
#endif
    );

    return TxSetBuilder::writeSelection(validatedPhases, app, ctx,
                                        closeTimeOffset, onTxSetReady,
                                        deferredSelfCheck
#ifdef BUILD_TESTS
                                        ,
                                        enforceTxsApplyOrder
#endif
    );
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
TxSetBuilder::writeSelection(
    std::vector<TxSetPhaseFrame> const& validatedPhases, Application& app,
    TxSetValidationContext const& ctx, ApplyTimeOffset closeTimeOffset,
    TxSetReadyCallback const& onTxSetReady,
    std::shared_ptr<TxSetSelfCheck const>* deferredSelfCheck
#ifdef BUILD_TESTS
    ,
    bool enforceTxsApplyOrder
#endif
)
{
    releaseAssert(threadIsMain());
    releaseAssert(!app.getLedgerManager().isApplying());
    auto const& lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    bool const selfCheckIsDeferred = deferredSelfCheck
#ifdef BUILD_TESTS
                                     && !enforceTxsApplyOrder
#endif
        ;
    // The check of the written set comes after the set is in use. What the
    // selection itself can get wrong (two transactions of one account, fees,
    // limits, conflicting clusters) is checked before anything leaves this
    // function, as it always was: on another thread while this one writes
    // the set (both only read the selection), and waited for before the set
    // is handed to anyone.
    std::future<TxSetValidationResult> selectionCheck;
    if (selfCheckIsDeferred)
    {
        bool const isGeneralized = protocolVersionStartsFrom(
            lclHeader.header.ledgerVersion, SOROBAN_PROTOCOL_VERSION);
        // The two threads read the same frames. That is safe because the
        // check reads no lazily computed member of a frame, and the write
        // only reads full hashes, which every selected frame computed when
        // it was validated.
        auto check = [&validatedPhases, &app, &ctx, isGeneralized,
                      closeTimeOffset]() {
            return TxSetBuilder::checkSelection(validatedPhases, app, ctx,
                                                isGeneralized, closeTimeOffset);
        };
        try
        {
            selectionCheck = std::async(std::launch::async, check);
        }
        catch (std::system_error const&)
        {
            // No thread to be had: check here.
            std::promise<TxSetValidationResult> done;
            done.set_value(check());
            selectionCheck = done.get_future();
        }
    }
    auto xdrTimer =
        app.getMetrics().NewTimer({"herder", "txset", "build-xdr"}).TimeScope();
    // Preliminary applicable frame - we don't know the contents hash yet, but
    // we also don't return this.
    std::unique_ptr<ApplicableTxSetFrame> preliminaryApplicableTxSet(
        new ApplicableTxSetFrame(app, lclHeader, validatedPhases,
                                 std::nullopt));

    // Do the roundtrip through XDR to ensure we never build an incorrect tx set
    // for nomination.
    auto outputTxSet = preliminaryApplicableTxSet->toWireTxSetFrame();
#ifdef BUILD_TESTS
    if (enforceTxsApplyOrder)
    {
        // Fill in the contents hash if we're skipping the normal roundtrip
        // and validation flow.
        preliminaryApplicableTxSet->mContentsHash =
            outputTxSet->getContentsHash();
        return std::make_pair(outputTxSet,
                              std::move(preliminaryApplicableTxSet));
    }
#endif

    xdrTimer.Stop();
    if (selectionCheck.valid())
    {
        auto const result = selectionCheck.get();
        if (result != TxSetValidationResult::VALID)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING(
                    "Created invalid tx set frame, validation result: {:s}"),
                toString(result)));
        }
    }
    // The final XDR is available now. Start proposal encoding while the
    // roundtrip and final validity checks below run; only the local builder
    // supplies this callback. The callback must not nominate the set.
    if (onTxSetReady)
    {
        onTxSetReady(outputTxSet);
    }

    if (deferredSelfCheck)
    {
        // What is cheap to check of the written set is checked here: the
        // shape of its XDR (no empty component, stage or cluster, ...), and
        // that it holds as many transactions as the selection.
        if (outputTxSet->isGeneralizedTxSet() &&
            !validateTxSetXDRStructure(outputTxSet->getGeneralizedXDR(),
                                       lclHeader.header.ledgerVersion))
        {
            throw std::runtime_error(
                "Couldn't prepare created tx set frame for apply");
        }
        size_t selected = 0;
        for (auto const& phase : validatedPhases)
        {
            selected += phase.sizeTx();
        }
        if (outputTxSet->sizeTxTotal() != selected)
        {
            throw std::runtime_error("Created invalid tx set frame - shape is "
                                     "mismatched after writing.");
        }
        // The caller runs the check (see TxSetSelfCheck) and goes on with
        // the set as selected, which is what the check compares the XDR
        // with.
        *deferredSelfCheck = TxSetBuilder::makeSelfCheck(
            app, outputTxSet, lclHeader, validatedPhases, closeTimeOffset);
        preliminaryApplicableTxSet->mContentsHash =
            outputTxSet->getContentsHash();
        return std::make_pair(outputTxSet,
                              std::move(preliminaryApplicableTxSet));
    }

    std::vector<size_t> phaseSizes;
    for (auto const& phase : validatedPhases)
    {
        phaseSizes.push_back(phase.sizeTx());
    }
    ApplicableTxSetFrameConstPtr outputApplicableTxSet;
    // We already trimmed invalid transactions in an earlier call to
    // `trimInvalid`, so the check skips transaction validation.
    auto const failure = TxSetBuilder::checkBuiltTxSet(
        app, *outputTxSet, TxSetValidationContext::forLCL(app), phaseSizes,
        closeTimeOffset.seconds(), &outputApplicableTxSet);
    if (failure)
    {
        throw std::runtime_error(*failure);
    }

    return std::make_pair(outputTxSet, std::move(outputApplicableTxSet));
}

uint32_t
TxSetBuildLimits::maxTxSetSizeOps() const
{
    // As LedgerManager::getLastMaxTxSetSizeOps.
    auto const n = mLclHeader.maxTxSetSize;
    return protocolVersionStartsFrom(mLclHeader.ledgerVersion,
                                     ProtocolVersion::V_11)
               ? n
               : (n * MAX_OPS_PER_TX);
}

bool
TxSetBuildLimits::sameAs(TxSetBuildLimits const& other) const
{
    auto const& a = mLclHeader;
    auto const& b = other.mLclHeader;
    if (a.ledgerVersion != b.ledgerVersion || a.baseFee != b.baseFee ||
        a.baseReserve != b.baseReserve || a.maxTxSetSize != b.maxTxSetSize)
    {
        return false;
    }
    if ((mSorobanConfig == nullptr) != (other.mSorobanConfig == nullptr))
    {
        return false;
    }
    if (!mSorobanConfig)
    {
        return true;
    }
    auto const& s = *mSorobanConfig;
    auto const& t = *other.mSorobanConfig;
    return s.maxLedgerResources() == t.maxLedgerResources() &&
           s.ledgerMaxInstructions() == t.ledgerMaxInstructions() &&
           s.ledgerMaxDependentTxClusters() ==
               t.ledgerMaxDependentTxClusters() &&
           s.ledgerMaxTxCount() == t.ledgerMaxTxCount();
}

TxSetBuildLimits
TxSetBuildLimits::forLCL(Application& app)
{
    releaseAssert(threadIsMain());
    auto& lm = app.getLedgerManager();
    TxSetBuildLimits limits;
    limits.mLclHeader = lm.getLastClosedLedgerHeader().header;
    if (protocolVersionStartsFrom(limits.mLclHeader.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        limits.mSorobanConfig = &lm.getLastClosedSorobanNetworkConfig();
    }
    return limits;
}

std::vector<TxSetPhaseFrame>
TxSetBuilder::selectPhases(
    PerPhaseTransactionList const& txPhases, Application& app,
    TxSetBuildLimits const& limits, TxSetValidationContext const& ctx,
    ApplyTimeOffset closeTimeOffset, PerPhaseTransactionList& invalidTxs,
    std::vector<size_t> const& validTxTargets, std::string const& timerPrefix,
    std::optional<size_t> tieBreakSeed,
    std::vector<std::vector<bool>>* hadTxNotFittingLane
#ifdef BUILD_TESTS
    ,
    bool enforceTxsApplyOrder,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder,
    bool disableTxValidationForLegacyScenario
#endif
)
{
    releaseAssert(txPhases.size() == invalidTxs.size());
    if (hadTxNotFittingLane)
    {
        hadTxNotFittingLane->assign(txPhases.size(), {});
    }
    releaseAssert(validTxTargets.empty() ||
                  validTxTargets.size() == txPhases.size());
    releaseAssert(txPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));

    auto const& cfg = app.getConfig();
    std::vector<TxSetPhaseFrame> validatedPhases;
    UnorderedMap<AccountID, int64_t> accountFeeMap;
    for (size_t i = 0; i < txPhases.size(); ++i)
    {
        auto const& phaseTxs = txPhases[i];
        bool expectSoroban = static_cast<TxSetPhase>(i) == TxSetPhase::SOROBAN;
        if (!std::all_of(phaseTxs.begin(), phaseTxs.end(), [&](auto const& tx) {
                return tx->isSoroban() == expectSoroban;
            }))
        {
            throw std::runtime_error("TxSetFrame::makeFromTransactions: phases "
                                     "contain txs of wrong type");
        }

        auto& invalid = invalidTxs[i];
        TxFrameList validatedTxs;
#ifdef BUILD_TESTS
        if (disableTxValidationForLegacyScenario)
        {
            REQUIRE(protocolVersionIsBefore(limits.mLclHeader.ledgerVersion,
                                            ProtocolVersion::V_20));
            validatedTxs = phaseTxs;
        }
        else
        {
#endif
            auto trimTimer =
                app.getMetrics()
                    .NewTimer({"herder", "txset", timerPrefix + "-trim"})
                    .TimeScope();
            if (validTxTargets.empty())
            {
                validatedTxs =
                    TxSetUtils::trimInvalid(phaseTxs, app, ctx, accountFeeMap,
                                            closeTimeOffset.seconds(),
                                            closeTimeOffset.seconds(), invalid);
            }
            else
            {
                // Transactions of limited lanes (DEX) can't fill the phase,
                // so only generic lane ones count toward the target.
                auto laneConfig = createSurgePricingLaneConfig(
                    static_cast<TxSetPhase>(i), cfg, limits);
                validatedTxs = TxSetUtils::trimInvalidPrefix(
                    phaseTxs, validTxTargets[i],
                    [&laneConfig](TransactionFrameBase const& tx) {
                        return laneConfig->getLane(tx) ==
                               SurgePricingPriorityQueue::GENERIC_LANE;
                    },
                    app, ctx, accountFeeMap, closeTimeOffset.seconds(),
                    closeTimeOffset.seconds(), invalid);
            }
#ifdef BUILD_TESTS
            // In tests we shouldn't be really trying to close ledgers with
            // invalid transactions trimmed (which is unfortunately a very
            // common footgun that many tests have hit).
            // However, enforcing that generically is quite involved as test
            // builds are used for e.g. loadgen scenarios. This method is also
            // expectedly used in the tests that test tx trimming.
            // So for now we only ensure that a subset of test-only scenarios
            // which also historically had a lot of issues due to skipping
            // validation only deals with the valid transactions now.
            // Ideally we clean this up further in the future and ensure that
            // tests always close ledgers with valid transactions.
            if (enforceTxsApplyOrder)
            {
                REQUIRE(invalid.empty());
            }
        }
#endif
        auto phaseType = static_cast<TxSetPhase>(i);
        auto selectTimer =
            app.getMetrics()
                .NewTimer({"herder", "txset", timerPrefix + "-select"})
                .TimeScope();
        auto [includedTxs, inclusionFeeMapBinding] = applySurgePricing(
            phaseType, validatedTxs, cfg, limits,
            tieBreakSeed ? std::make_optional(*tieBreakSeed + i) : std::nullopt,
            hadTxNotFittingLane ? &(*hadTxNotFittingLane)[i] : nullptr
#ifdef BUILD_TESTS
            ,
            enforceTxsApplyOrder, parallelSorobanOrder
#endif
        );
        auto inclusionFeeMap = inclusionFeeMapBinding;
        std::visit(
            [&validatedPhases, phaseType, inclusionFeeMap](auto&& txs) {
                using T = std::decay_t<decltype(txs)>;
                if constexpr (std::is_same_v<T, TxFrameList>)
                {
                    validatedPhases.emplace_back(
                        TxSetPhaseFrame(phaseType, txs, inclusionFeeMap));
                }
                else if constexpr (std::is_same_v<T, TxStageFrameList>)
                {
                    validatedPhases.emplace_back(TxSetPhaseFrame(
                        phaseType, std::move(txs), inclusionFeeMap));
                }
                else
                {
                    // This can't be just `false` as if an assertion is not
                    // dependent on template argument, it will be
                    // unconditionally triggered.
                    static_assert(!std::is_same_v<T, T>,
                                  "Non-exhaustive visitor");
                }
            },
            includedTxs);
    }
    return validatedPhases;
}

TxSetValidationResult
TxSetBuilder::checkSelection(std::vector<TxSetPhaseFrame> const& phases,
                             Application& app,
                             TxSetValidationContext const& ctx,
                             bool isGeneralized,
                             ApplyTimeOffset closeTimeOffset)
{
    ZoneScoped;
    auto timer = app.getMetrics()
                     .NewTimer({"herder", "txset", "build-selection-check"})
                     .TimeScope();
    // The transactions were validated by the selection.
    return ApplicableTxSetFrame::checkPhasesValid(
        app, ctx, phases, isGeneralized, closeTimeOffset.seconds(),
        closeTimeOffset.seconds(), /* txsAreValidated */ true);
}

namespace
{
// Removes `gone` from a phase's transactions, and the clusters and stages
// that are left empty with them.
template <typename Gone>
TxStageFrameList
withoutTxs(TxStageFrameList const& stages, Gone const& gone)
{
    TxStageFrameList res;
    for (auto const& stage : stages)
    {
        TxStageFrame keptStage;
        for (auto const& cluster : stage)
        {
            TxClusterFrame keptCluster;
            for (auto const& tx : cluster)
            {
                if (gone.count(tx.get()) == 0)
                {
                    keptCluster.push_back(tx);
                }
            }
            if (!keptCluster.empty())
            {
                keptStage.emplace_back(std::move(keptCluster));
            }
        }
        if (!keptStage.empty())
        {
            res.emplace_back(std::move(keptStage));
        }
    }
    return res;
}
}

std::vector<TxSetPhaseFrame>
TxSetBuilder::completeSelection(
    std::vector<TxSetPhaseFrame> const& selected,
    std::vector<std::vector<bool>> const& hadTxNotFittingLane,
    TxSetCandidateSource const& moreCandidates, Application& app,
    TxSetBuildLimits const& limits, TxSetValidationContext const& ctx,
    ApplyTimeOffset closeTimeOffset, PerPhaseTransactionList& invalidTxs,
    TxSetCompletionStats& stats, std::optional<size_t> tieBreakSeed)
{
    ZoneScoped;
    size_t const phaseCount = selected.size();
    releaseAssert(phaseCount == static_cast<size_t>(TxSetPhase::PHASE_COUNT));
    releaseAssert(hadTxNotFittingLane.size() == phaseCount);
    releaseAssert(invalidTxs.size() == phaseCount);
    releaseAssert(limits.mSorobanConfig);
    auto& metrics = app.getMetrics();
    auto const& cfg = app.getConfig();
    auto const offset = closeTimeOffset.seconds();
    auto const ledgerVersion = limits.mLclHeader.ledgerVersion;

    struct PhaseState
    {
        std::shared_ptr<SurgePricingLaneConfig> mLaneConfig;
        bool mParallel{false};
        // What is selected so far, in one of the two shapes.
        TxFrameList mSequential;
        TxStageFrameList mStages;
        std::optional<ParallelPhaseExtender> mExtender;
        // What the selection reported, per surge pricing lane.
        std::vector<bool> mHadTxNotFittingLane;
        // The same for the completion: a valid candidate was left out.
        std::vector<bool> mLeftOutLanes;
        bool mLeftOut{false};
        // A sequential phase was selected again.
        bool mReselected{false};
        size_t mRemoved{0};
        size_t mAdded{0};
        // Candidates were looked at for this phase during the completion.
        bool mExamined{false};
        // The phase lost transactions or candidates.
        bool mLost{false};
        // Transactions of this phase seen so far (selected or offered).
        size_t mSeen{0};
        // Batches from the mempool the phase has looked at.
        size_t mMempoolBatches{0};
        // Nothing more is looked for (see the top-up below).
        bool mFull{false};
        bool mStarved{false};
    };
    std::vector<PhaseState> states(phaseCount);
    // One fee-source total for the whole set, as in a build.
    UnorderedMap<AccountID, int64_t> accountFeeMap;
    // One transaction per source account, across the phases.
    UnorderedSet<AccountID> sources;

    auto atCapacity = [&](size_t p) {
        auto const& state = states[p];
        auto const limit = state.mLaneConfig->getLaneLimits().at(
            SurgePricingPriorityQueue::GENERIC_LANE);
        int64_t ops = 0;
        if (state.mExtender)
        {
            ops = static_cast<int64_t>(state.mExtender->sizeTx());
        }
        else if (state.mParallel)
        {
            for (auto const& stage : state.mStages)
            {
                for (auto const& cluster : stage)
                {
                    ops += static_cast<int64_t>(cluster.size());
                }
            }
        }
        else
        {
            ops = static_cast<int64_t>(countOps(state.mSequential));
        }
        return ops >= limit.getVal(Resource::Type::OPERATIONS);
    };

    // 1. The validity pass. Whatever the selection was made on, every one of
    // its transactions is validated here against the ledger the set goes on
    // top of, by the function that validates a set a node receives; what
    // fails leaves the selection.
    {
        auto timer = metrics.NewTimer({"herder", "txset", "early-final-pass"})
                         .TimeScope();
        for (size_t p = 0; p < phaseCount; ++p)
        {
            auto const& phase = selected[p];
            auto& state = states[p];
            state.mLaneConfig = createSurgePricingLaneConfig(
                static_cast<TxSetPhase>(p), cfg, limits);
            state.mParallel = phase.isParallel();
            state.mHadTxNotFittingLane = hadTxNotFittingLane[p];
            releaseAssert(state.mHadTxNotFittingLane.size() ==
                          state.mLaneConfig->getLaneLimits().size());
            state.mLeftOutLanes.assign(state.mHadTxNotFittingLane.size(),
                                       false);
            stats.mSelected += phase.sizeTx();
            auto invalid = TxSetUtils::getInvalidTxListWithErrors(
                               phase, app, ctx, accountFeeMap, offset, offset)
                               .first;
            UnorderedSet<TransactionFrameBase const*> gone;
            for (auto const& tx : invalid)
            {
                gone.insert(tx.get());
            }
            state.mRemoved = gone.size();
            stats.mRemoved += gone.size();
            if (state.mParallel)
            {
                state.mStages = withoutTxs(phase.getParallelStages(), gone);
            }
            else
            {
                for (auto const& tx : phase.getSequentialTxs())
                {
                    if (gone.count(tx.get()) == 0)
                    {
                        state.mSequential.push_back(tx);
                    }
                }
            }
            for (auto const& tx : phase)
            {
                if (gone.count(tx.get()) == 0)
                {
                    sources.insert(tx->getSourceID());
                }
            }
            invalidTxs[p] = std::move(invalid);
            // Only a phase at its capacity is full from the start. That the
            // selection left a transaction out says little: one that did
            // not fit where it had to go (a cluster, a lane of its own)
            // leaves room for others.
            state.mFull = atCapacity(p);
        }
    }

    // 2. The top-up: while a phase has room, take further candidates, each
    // validated like the selection's own.
    //
    // A phase stops asking when
    //  - it is at its capacity;
    //  - valid candidates were offered and none could be added (any number
    //    of them from the mempool, several of the selection's leftovers): the
    //    room it has left is of no use to what is on offer;
    //  - it has seen the mempool's top once, unless it lost candidates
    //    (transactions removed by the validity pass, candidates that were
    //    invalid, of an account already in the set, or did not fit) and
    //    that top held at least a phase's capacity of its kind: then it
    //    goes on to deeper pulls, as long as each brings something new for
    //    it. A phase that is short without having lost anything got all
    //    the mempool's top holds for it.
    size_t const MANY_LEFT_OUT = 8;
    auto capacityOf = [&](size_t p) {
        return static_cast<size_t>(std::max<int64_t>(
            0, states[p]
                   .mLaneConfig->getLaneLimits()
                   .at(SurgePricingPriorityQueue::GENERIC_LANE)
                   .getVal(Resource::Type::OPERATIONS)));
    };
    for (size_t p = 0; p < phaseCount; ++p)
    {
        states[p].mLost = states[p].mRemoved != 0;
        states[p].mSeen = selected[p].sizeTx();
    }
    // Selects a sequential phase again, from what it holds and `extra`;
    // returns how many of `extra` are in.
    auto reselect = [&](size_t p, TxFrameList const& extra) {
        auto& state = states[p];
        TxFrameList all = state.mSequential;
        all.insert(all.end(), extra.begin(), extra.end());
        std::vector<bool> flags;
        auto [included, feeMap] = applySurgePricing(
            static_cast<TxSetPhase>(p), all, cfg, limits,
            tieBreakSeed ? std::make_optional(*tieBreakSeed + p) : std::nullopt,
            &flags
#ifdef BUILD_TESTS
            ,
            false, {}
#endif
        );
        state.mSequential = std::get<TxFrameList>(included);
        state.mReselected = true;
        releaseAssert(flags.size() == state.mLeftOutLanes.size());
        for (size_t lane = 0; lane < flags.size(); ++lane)
        {
            state.mLeftOutLanes[lane] =
                state.mLeftOutLanes[lane] || flags[lane];
        }
        UnorderedSet<TransactionFrameBase const*> in;
        for (auto const& tx : state.mSequential)
        {
            in.insert(tx.get());
        }
        size_t added = 0;
        for (auto const& tx : extra)
        {
            if (in.count(tx.get()) != 0)
            {
                sources.insert(tx->getSourceID());
                ++added;
            }
        }
        return added;
    };
    auto wantsMore = [&]() {
        return std::any_of(
            states.begin(), states.end(),
            [](PhaseState const& s) { return !s.mFull && !s.mStarved; });
    };
    {
        auto timer =
            metrics.NewTimer({"herder", "txset", "early-top-up"}).TimeScope();
        while (wantsMore())
        {
            auto batch = moreCandidates();
            if (!batch)
            {
                break;
            }
            stats.mCandidates += batch->mTxs.size();
            TxFrameList classicTxs;
            TxFrameList sorobanTxs;
            std::vector<size_t> offered(phaseCount, 0);
            for (auto const& tx : batch->mTxs)
            {
                size_t const p = tx->isSoroban() ? 1 : 0;
                if (states[p].mFull)
                {
                    continue;
                }
                ++offered[p];
                if (sources.count(tx->getSourceID()) != 0)
                {
                    continue;
                }
                (p == 1 ? sorobanTxs : classicTxs).push_back(tx);
            }
            TxSetUtils::keepOnePerSourceAccount(classicTxs, sorobanTxs);
            for (size_t p = 0; p < phaseCount; ++p)
            {
                auto& state = states[p];
                if (state.mFull)
                {
                    continue;
                }
                // The selection's own verdict on what it left out is void
                // from here on: the candidates are judged on this ledger.
                state.mExamined = true;
                state.mSeen += offered[p];
                auto const& candidates = p == 1 ? sorobanTxs : classicTxs;
                size_t valid = 0;
                size_t added = 0;
                if (!candidates.empty())
                {
                    TxFrameList invalid;
                    auto const validTxs = TxSetUtils::trimInvalid(
                        candidates, app, ctx, accountFeeMap, offset, offset,
                        invalid);
                    invalidTxs[p].insert(invalidTxs[p].end(), invalid.begin(),
                                         invalid.end());
                    valid = validTxs.size();
                    if (valid != 0 && state.mParallel)
                    {
                        if (!state.mExtender)
                        {
                            state.mExtender.emplace(
                                std::move(state.mStages),
                                *limits.mSorobanConfig,
                                state.mLaneConfig->getLaneLimits().at(
                                    SurgePricingPriorityQueue::GENERIC_LANE),
                                ledgerVersion);
                        }
                        for (auto const& tx : validTxs)
                        {
                            if (state.mExtender->tryAdd(tx))
                            {
                                sources.insert(tx->getSourceID());
                                ++added;
                            }
                        }
                    }
                    else if (valid != 0)
                    {
                        // A sequential phase is cheap to select: again, by
                        // the standard surge pricing.
                        added = reselect(p, validTxs);
                    }
                }
                state.mAdded += added;
                // A valid candidate that is not in: the phase's
                // transactions competed for its room.
                state.mLeftOut = state.mLeftOut || added < valid;
                state.mLost = state.mLost || added < offered[p];
                if (atCapacity(p) ||
                    (valid != 0 && added == 0 &&
                     (batch->mFromMempool || valid >= MANY_LEFT_OUT)))
                {
                    state.mFull = true;
                }
                else if (batch->mFromMempool)
                {
                    ++state.mMempoolBatches;
                    state.mStarved =
                        state.mMempoolBatches == 1
                            ? !(state.mLost && state.mSeen >= capacityOf(p))
                            : offered[p] == 0;
                }
            }
        }
    }

    // 3. The phases as they are now. Base fees by the rule of a build:
    // transactions of a lane compete on fees only if a valid one was left
    // out. For a phase that looked at candidates here, that is what
    // happened here; otherwise it is what the selection reported.
    std::vector<TxSetPhaseFrame> res;
    for (size_t p = 0; p < phaseCount; ++p)
    {
        auto& state = states[p];
        auto const phaseType = static_cast<TxSetPhase>(p);
        stats.mAdded += state.mAdded;
        std::vector<bool> flags = state.mHadTxNotFittingLane;
        if (state.mExamined || state.mRemoved != 0)
        {
            flags = state.mLeftOutLanes;
            if (state.mParallel)
            {
                flags.at(SurgePricingPriorityQueue::GENERIC_LANE) =
                    state.mLeftOut;
            }
        }
        if (state.mRemoved == 0 && state.mAdded == 0 && !state.mReselected &&
            flags == state.mHadTxNotFittingLane)
        {
            // Untouched.
            res.emplace_back(selected[p]);
            continue;
        }
        if (state.mParallel)
        {
            auto stages = state.mExtender ? state.mExtender->getStages()
                                          : std::move(state.mStages);
            std::variant<TxFrameList, TxStageFrameList> included = stages;
            auto feeMap =
                computeInclusionFeeMap(phaseType, included, *state.mLaneConfig,
                                       limits.mLclHeader, flags);
            res.emplace_back(
                TxSetPhaseFrame(phaseType, std::move(stages), feeMap));
        }
        else
        {
            if (!state.mReselected)
            {
                // Transactions were removed and none added: select what is
                // left.
                reselect(p, {});
                flags = state.mLeftOutLanes;
            }
            std::variant<TxFrameList, TxStageFrameList> included =
                state.mSequential;
            auto feeMap =
                computeInclusionFeeMap(phaseType, included, *state.mLaneConfig,
                                       limits.mLclHeader, flags);
            res.emplace_back(
                TxSetPhaseFrame(phaseType, state.mSequential, feeMap));
        }
    }
    return res;
}

std::shared_ptr<TxSetSelfCheck const>
TxSetBuilder::makeSelfCheck(Application& app,
                            TxSetXDRFrameConstPtr const& txSet,
                            LedgerHeaderHistoryEntry const& lcl,
                            std::vector<TxSetPhaseFrame> const& phases,
                            ApplyTimeOffset closeTimeOffset)
{
    releaseAssert(threadIsMain());
    std::shared_ptr<TxSetSelfCheck> check(new TxSetSelfCheck());
    check->mTxSet = txSet;
    check->mLcl = lcl;
    if (protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        check->mSorobanConfig = std::make_shared<SorobanNetworkConfig const>(
            app.getLedgerManager().getLastClosedSorobanNetworkConfig());
    }
    for (auto const& phase : phases)
    {
        check->mPhaseSizes.push_back(phase.sizeTx());
    }
    check->mCloseTimeOffset = closeTimeOffset.seconds();
    return check;
}

std::optional<std::string>
TxSetBuilder::checkBuiltTxSet(Application& app, TxSetXDRFrame const& txSet,
                              TxSetValidationContext const& ctx,
                              std::vector<size_t> const& phaseSizes,
                              uint64_t closeTimeOffset,
                              ApplicableTxSetFrameConstPtr* roundTripped)
{
    ZoneScoped;
    auto roundtripTimer = app.getMetrics()
                              .NewTimer({"herder", "txset", "build-roundtrip"})
                              .TimeScope();
    ApplicableTxSetFrameConstPtr applicable =
        txSet.prepareForApply(app, ctx.mLcl.header);
    roundtripTimer.Stop();

    if (!applicable)
    {
        return "Couldn't prepare created tx set frame for apply";
    }

    // Make sure no transactions were lost during the roundtrip and the output
    // tx set is valid.
    bool valid = phaseSizes.size() == applicable->numPhases();
    if (valid)
    {
        for (size_t i = 0; i < phaseSizes.size(); ++i)
        {
            valid = valid && phaseSizes[i] ==
                                 applicable->sizeTx(static_cast<TxSetPhase>(i));
        }
    }
    if (!valid)
    {
        return "Created invalid tx set frame - shape is mismatched after "
               "roundtrip.";
    }
    auto finalTimer = app.getMetrics()
                          .NewTimer({"herder", "txset", "build-final-check"})
                          .TimeScope();
    auto validationResult = applicable->checkValidInternalWithResult(
        app, ctx, closeTimeOffset, closeTimeOffset, true);
    finalTimer.Stop();
    if (validationResult != TxSetValidationResult::VALID)
    {
        return fmt::format(
            FMT_STRING("Created invalid tx set frame, validation result: {:s}"),
            toString(validationResult));
    }
    if (roundTripped)
    {
        *roundTripped = std::move(applicable);
    }
    return std::nullopt;
}

std::optional<std::string>
TxSetSelfCheck::run(Application& app) const
{
    // The check does not validate transactions, so it never asks for a view
    // of the ledger.
    TxSetValidationContext const ctx{
        mLcl, mSorobanConfig.get(),
        []() -> std::unique_ptr<AbstractLedgerView const> {
            throw std::runtime_error(
                "the tx set self-check does not read the ledger");
        },
        1};
    try
    {
        return TxSetBuilder::checkBuiltTxSet(app, *mTxSet, ctx, mPhaseSizes,
                                             mCloseTimeOffset, nullptr);
    }
    catch (std::exception const& e)
    {
        return fmt::format(FMT_STRING("tx set self-check threw: {}"), e.what());
    }
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeEmpty(Hash const& previousLedgerHash,
                         uint32 previousLedgerVersion)
{
    if (protocolVersionStartsFrom(previousLedgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        bool isParallelSoroban = false;
        isParallelSoroban = protocolVersionStartsFrom(
            previousLedgerVersion, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
        std::vector<TxSetPhaseFrame> emptyPhases = {
            TxSetPhaseFrame::makeEmpty(TxSetPhase::CLASSIC, false),
            TxSetPhaseFrame::makeEmpty(TxSetPhase::SOROBAN, isParallelSoroban)};

        GeneralizedTransactionSet txSet;
        transactionsToGeneralizedTransactionSetXDR(emptyPhases,
                                                   previousLedgerHash, txSet);
        return TxSetXDRFrame::makeFromWire(std::move(txSet));
    }
    TransactionSet txSet;
    transactionsToTransactionSetXDR({}, previousLedgerHash, txSet);
    return TxSetXDRFrame::makeFromWire(std::move(txSet));
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeEmpty(LedgerHeaderHistoryEntry const& lclHeader)
{
    return makeEmpty(lclHeader.hash, lclHeader.header.ledgerVersion);
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                           TxFrameList const& txs)
{
    TransactionSet txSet;
    transactionsToTransactionSetXDR(txs, previousLedgerHash, txSet);
    return TxSetXDRFrame::makeFromWire(std::move(txSet));
}

#ifdef BUILD_TESTS
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    TxFrameList txs, Application& app, ApplyTimeOffset closeTimeOffset,
    bool enforceTxsApplyOrder,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder,
    bool disableTxValidationForLegacyScenario)
{
    TxFrameList invalid;
    return makeTxSetFromTransactions(txs, app, closeTimeOffset, invalid,
                                     enforceTxsApplyOrder, parallelSorobanOrder,
                                     disableTxValidationForLegacyScenario);
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    TxFrameList txs, Application& app, ApplyTimeOffset closeTimeOffset,
    TxFrameList& invalidTxs, bool enforceTxsApplyOrder,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder,
    bool disableTxValidationForLegacyScenario)
{
    releaseAssert(threadIsMain());
    releaseAssert(!app.getLedgerManager().isApplying());
    auto lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    PerPhaseTransactionList perPhaseTxs;
    perPhaseTxs.resize(protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                                 SOROBAN_PROTOCOL_VERSION)
                           ? 2
                           : 1);
    for (auto& tx : txs)
    {
        if (tx->isSoroban())
        {
            perPhaseTxs[static_cast<size_t>(TxSetPhase::SOROBAN)].push_back(tx);
        }
        else
        {
            perPhaseTxs[static_cast<size_t>(TxSetPhase::CLASSIC)].push_back(tx);
        }
    }
    PerPhaseTransactionList invalid;
    invalid.resize(perPhaseTxs.size());
    auto res = makeTxSetFromTransactions(
        perPhaseTxs, app, closeTimeOffset, invalid, enforceTxsApplyOrder,
        parallelSorobanOrder, disableTxValidationForLegacyScenario);
    if (enforceTxsApplyOrder)
    {
        auto const& resPhases = res.second->getPhases();
        std::vector<TxSetPhaseFrame> overridePhases;
        for (size_t i = 0; i < resPhases.size(); ++i)
        {
            if (resPhases[i].isParallel())
            {
                TxStageFrameList parallelStages =
                    resPhases[i].getParallelStages();
                overridePhases.emplace_back(TxSetPhaseFrame(
                    static_cast<TxSetPhase>(i), std::move(parallelStages),
                    std::make_shared<InclusionFeeMap>(
                        resPhases[i].getInclusionFeeMap())));
            }
            else
            {
                overridePhases.emplace_back(TxSetPhaseFrame(
                    static_cast<TxSetPhase>(i), std::move(perPhaseTxs[i]),
                    std::make_shared<InclusionFeeMap>(
                        resPhases[i].getInclusionFeeMap())));
            }
        }
        res.second->mApplyOrderPhases = overridePhases;
        res.first->mApplicableTxSetOverride = std::move(res.second);
    }
    invalidTxs = invalid[0];
    return res;
}

StellarMessage
TxSetXDRFrame::toStellarMessage() const
{
    StellarMessage newMsg;
    if (isGeneralizedTxSet())
    {
        newMsg.type(GENERALIZED_TX_SET);
        toXDR(newMsg.generalizedTxSet());
    }
    else
    {
        newMsg.type(TX_SET);
        toXDR(newMsg.txSet());
    }
    return newMsg;
}

#endif

ApplicableTxSetFrameConstPtr
TxSetXDRFrame::prepareForApply(Application& app,
                               LedgerHeader const& lclHeader) const
{
#ifdef BUILD_TESTS
    if (mApplicableTxSetOverride)
    {
        return ApplicableTxSetFrameConstPtr(
            new ApplicableTxSetFrame(*mApplicableTxSetOverride));
    }
#endif
    ZoneScoped;
    std::vector<TxSetPhaseFrame> phaseFrames;
    if (isGeneralizedTxSet())
    {
        auto const& xdrTxSet = std::get<GeneralizedTransactionSet>(mXDRTxSet);
        if (!validateTxSetXDRStructure(xdrTxSet, lclHeader.ledgerVersion))
        {
            CLOG_DEBUG(Herder,
                       "Got bad generalized txSet with invalid XDR structure");
            return nullptr;
        }
        auto const& xdrPhases = xdrTxSet.v1TxSet().phases;

        for (size_t phaseId = 0; phaseId < xdrPhases.size(); ++phaseId)
        {
            auto maybePhase = TxSetPhaseFrame::makeFromWire(
                static_cast<TxSetPhase>(phaseId), app.getNetworkID(),
                xdrPhases[phaseId]);
            if (!maybePhase)
            {
                return nullptr;
            }
            phaseFrames.emplace_back(std::move(*maybePhase));
        }
    }
    else
    {
        auto const& xdrTxSet = std::get<TransactionSet>(mXDRTxSet);
        auto maybePhase = TxSetPhaseFrame::makeFromWireLegacy(
            lclHeader, app.getNetworkID(), xdrTxSet.txs);
        if (!maybePhase)
        {
            return nullptr;
        }
        phaseFrames.emplace_back(std::move(*maybePhase));
    }
    return std::unique_ptr<ApplicableTxSetFrame>(new ApplicableTxSetFrame(
        app, isGeneralizedTxSet(), previousLedgerHash(), phaseFrames, mHash));
}

bool
TxSetXDRFrame::isGeneralizedTxSet() const
{
    return std::holds_alternative<GeneralizedTransactionSet>(mXDRTxSet);
}

Hash const&
TxSetXDRFrame::getContentsHash() const
{
    return mHash;
}

Hash const&
TxSetXDRFrame::previousLedgerHash() const
{
    if (isGeneralizedTxSet())
    {
        return std::get<GeneralizedTransactionSet>(mXDRTxSet)
            .v1TxSet()
            .previousLedgerHash;
    }
    return std::get<TransactionSet>(mXDRTxSet).previousLedgerHash;
}

size_t
TxSetXDRFrame::sizeTxTotal() const
{
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        size_t totalSize = 0;
        for (auto const& phase : txSet.phases)
        {
            switch (phase.v())
            {
            case 0:
                for (auto const& component : phase.v0Components())
                {
                    totalSize += component.txsMaybeDiscountedFee().txs.size();
                }
                break;
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& cluster : stage)
                    {
                        totalSize += cluster.size();
                    }
                }
                break;
            default:
                break;
            }
        }
        return totalSize;
    }
    else
    {
        return std::get<TransactionSet>(mXDRTxSet).txs.size();
    }
}

size_t
TxSetXDRFrame::sizeOpTotalForLogging() const
{
    auto accumulateTxsFn = [](size_t sz, TransactionEnvelope const& tx) {
        size_t txOps = 0;
        switch (tx.type())
        {
        case ENVELOPE_TYPE_TX_V0:
            txOps = tx.v0().tx.operations.size();
            break;
        case ENVELOPE_TYPE_TX:
            txOps = tx.v1().tx.operations.size();
            break;
        case ENVELOPE_TYPE_TX_FEE_BUMP:
            txOps = 1 + tx.feeBump().tx.innerTx.v1().tx.operations.size();
            break;
        default:
            break;
        }
        return sz + txOps;
    };
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        size_t totalSize = 0;
        for (auto const& phase : txSet.phases)
        {
            switch (phase.v())
            {
            case 0:
                for (auto const& component : phase.v0Components())
                {
                    totalSize += std::accumulate(
                        component.txsMaybeDiscountedFee().txs.begin(),
                        component.txsMaybeDiscountedFee().txs.end(), 0ull,
                        accumulateTxsFn);
                }
                break;
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& cluster : stage)
                    {
                        totalSize +=
                            std::accumulate(cluster.begin(), cluster.end(),
                                            0ull, accumulateTxsFn);
                    }
                }
                break;
            default:
                break;
            }
        }
        return totalSize;
    }
    else
    {
        auto const& txs = std::get<TransactionSet>(mXDRTxSet).txs;
        return std::accumulate(txs.begin(), txs.end(), 0ull, accumulateTxsFn);
    }
}

PerPhaseTransactionList
TxSetXDRFrame::createTransactionFrames(Hash const& networkID) const
{
    PerPhaseTransactionList phaseTxs;
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        for (auto const& phase : txSet.phases)
        {
            auto& txs = phaseTxs.emplace_back();
            switch (phase.v())
            {
            case 0:
                for (auto const& component : phase.v0Components())
                {
                    for (auto const& tx : component.txsMaybeDiscountedFee().txs)
                    {
                        txs.emplace_back(
                            TransactionFrameBase::makeTransactionFromWire(
                                networkID, tx));
                    }
                }
                break;
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& cluster : stage)
                    {
                        for (auto const& tx : cluster)
                        {
                            txs.emplace_back(
                                TransactionFrameBase::makeTransactionFromWire(
                                    networkID, tx));
                        }
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    else
    {
        auto& txs = phaseTxs.emplace_back();
        auto const& txSet = std::get<TransactionSet>(mXDRTxSet).txs;
        for (auto const& tx : txSet)
        {
            txs.emplace_back(
                TransactionFrameBase::makeTransactionFromWire(networkID, tx));
        }
    }
    return phaseTxs;
}

void
TxSetXDRFrame::forEachTransactionEnvelope(
    std::function<void(TransactionEnvelope const&)> const& f) const
{
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        for (auto const& phase : txSet.phases)
        {
            switch (phase.v())
            {
            case 0:
                for (auto const& component : phase.v0Components())
                {
                    for (auto const& tx : component.txsMaybeDiscountedFee().txs)
                    {
                        f(tx);
                    }
                }
                break;
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& cluster : stage)
                    {
                        for (auto const& tx : cluster)
                        {
                            f(tx);
                        }
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    else
    {
        for (auto const& tx : std::get<TransactionSet>(mXDRTxSet).txs)
        {
            f(tx);
        }
    }
}

std::vector<Hash>
TxSetXDRFrame::getTransactionFullHashes() const
{
    ZoneScoped;
    std::vector<Hash> hashes;
    hashes.reserve(sizeTxTotal());
    // Same digest as TransactionFrame::getFullHash and
    // FeeBumpTransactionFrame::getFullHash: SHA-256 of the envelope's XDR.
    forEachTransactionEnvelope([&hashes](TransactionEnvelope const& tx) {
        hashes.emplace_back(xdrSha256(tx));
    });
    return hashes;
}

size_t
TxSetXDRFrame::encodedSize() const
{
    return mEncodedSize;
}

void
TxSetXDRFrame::toXDR(TransactionSet& txSet) const
{
    releaseAssert(!isGeneralizedTxSet());
    txSet = std::get<TransactionSet>(mXDRTxSet);
}

void
TxSetXDRFrame::toXDR(GeneralizedTransactionSet& txSet) const
{
    releaseAssert(isGeneralizedTxSet());
    txSet = std::get<GeneralizedTransactionSet>(mXDRTxSet);
}

GeneralizedTransactionSet const&
TxSetXDRFrame::getGeneralizedXDR() const
{
    releaseAssert(isGeneralizedTxSet());
    return std::get<GeneralizedTransactionSet>(mXDRTxSet);
}

void
TxSetXDRFrame::storeXDR(StoredTransactionSet& txSet) const
{
    if (isGeneralizedTxSet())
    {
        txSet.v(1);
        txSet.generalizedTxSet() =
            std::get<GeneralizedTransactionSet>(mXDRTxSet);
    }
    else
    {
        txSet.v(0);
        txSet.txSet() = std::get<TransactionSet>(mXDRTxSet);
    }
}

TxSetPhaseFrame::Iterator::Iterator(TxStageFrameList const& txs,
                                    size_t stageIndex)
    : mStages(txs), mStageIndex(stageIndex)
{
}

TransactionFrameBasePtr
TxSetPhaseFrame::Iterator::operator*() const
{

    if (mStageIndex >= mStages.size() ||
        mClusterIndex >= mStages[mStageIndex].size() ||
        mTxIndex >= mStages[mStageIndex][mClusterIndex].size())
    {
        throw std::runtime_error("TxPhase iterator out of bounds");
    }
    return mStages[mStageIndex][mClusterIndex][mTxIndex];
}

TxSetPhaseFrame::Iterator&
TxSetPhaseFrame::Iterator::operator++()
{
    if (mStageIndex >= mStages.size() ||
        mClusterIndex >= mStages[mStageIndex].size())
    {
        throw std::runtime_error("TxPhase iterator out of bounds");
    }
    ++mTxIndex;
    if (mTxIndex >= mStages[mStageIndex][mClusterIndex].size())
    {
        mTxIndex = 0;
        ++mClusterIndex;
        if (mClusterIndex >= mStages[mStageIndex].size())
        {
            mClusterIndex = 0;
            ++mStageIndex;
        }
    }
    return *this;
}

TxSetPhaseFrame::Iterator
TxSetPhaseFrame::Iterator::operator++(int)
{
    auto it = *this;
    ++(*this);
    return it;
}

bool
TxSetPhaseFrame::Iterator::operator==(Iterator const& other) const
{
    return mStageIndex == other.mStageIndex &&
           mClusterIndex == other.mClusterIndex && mTxIndex == other.mTxIndex &&
           // Make sure to compare the pointers, not the contents, both for
           // correctness and optimization.
           &mStages == &other.mStages;
}

bool
TxSetPhaseFrame::Iterator::operator!=(Iterator const& other) const
{
    return !(*this == other);
}

std::optional<TxSetPhaseFrame>
TxSetPhaseFrame::makeFromWire(TxSetPhase phase, Hash const& networkID,
                              TransactionPhase const& xdrPhase)
{
    auto inclusionFeeMapPtr = std::make_shared<InclusionFeeMap>();
    auto& inclusionFeeMap = *inclusionFeeMapPtr;
    std::optional<TxSetPhaseFrame> phaseFrame;
    switch (xdrPhase.v())
    {
    case 0:
    {
        TxFrameList txList;
        auto const& components = xdrPhase.v0Components();
        for (auto const& component : components)
        {
            switch (component.type())
            {
            case TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE:
                std::optional<int64_t> baseFee;
                if (component.txsMaybeDiscountedFee().baseFee)
                {
                    baseFee = *component.txsMaybeDiscountedFee().baseFee;
                    if (*baseFee < 0)
                    {
                        CLOG_DEBUG(Herder,
                                   "Got bad generalized txSet: component "
                                   "has negative base fee");
                        return std::nullopt;
                    }
                }
                size_t prevSize = txList.size();
                if (!addWireTxsToList(networkID,
                                      component.txsMaybeDiscountedFee().txs,
                                      txList))
                {
                    CLOG_DEBUG(Herder,
                               "Got bad generalized txSet: transactions "
                               "are not ordered correctly or contain "
                               "invalid transactions");
                    return std::nullopt;
                }
                for (auto it = txList.begin() + prevSize; it != txList.end();
                     ++it)
                {
                    inclusionFeeMap[*it] = baseFee;
                }
                break;
            }
        }
        phaseFrame.emplace(
            TxSetPhaseFrame(phase, std::move(txList), inclusionFeeMapPtr));
        break;
    }
    case 1:
    {
        auto const& xdrStages = xdrPhase.parallelTxsComponent().executionStages;
        std::optional<int64_t> baseFee;
        if (xdrPhase.parallelTxsComponent().baseFee)
        {
            baseFee = *xdrPhase.parallelTxsComponent().baseFee;
            if (*baseFee < 0)
            {
                CLOG_DEBUG(Herder, "Got bad generalized txSet: component "
                                   "has negative base fee");
                return std::nullopt;
            }
        }
        TxStageFrameList stages;
        stages.reserve(xdrStages.size());
        for (auto const& xdrStage : xdrStages)
        {
            auto& stage = stages.emplace_back();
            stage.reserve(xdrStage.size());
            for (auto const& xdrCluster : xdrStage)
            {
                auto& cluster = stage.emplace_back();
                cluster.reserve(xdrCluster.size());
                for (auto const& env : xdrCluster)
                {
                    auto tx = TransactionFrameBase::makeTransactionFromWire(
                        networkID, env);
                    if (!tx->XDRProvidesValidFee() ||
                        tx->getInclusionFee() <= 0)
                    {
                        CLOG_DEBUG(Herder, "Got bad generalized txSet: "
                                           "transaction has invalid XDR");
                        return std::nullopt;
                    }
                    cluster.push_back(tx);
                    inclusionFeeMap[tx] = baseFee;
                }
                if (!std::is_sorted(cluster.begin(), cluster.end(),
                                    &TxSetUtils::hashTxSorter))
                {
                    CLOG_DEBUG(Herder, "Got bad generalized txSet: "
                                       "cluster is not sorted");
                    return std::nullopt;
                }
            }
            if (!std::is_sorted(stage.begin(), stage.end(),
                                [](auto const& a, auto const& b) {
                                    releaseAssert(!a.empty() && !b.empty());
                                    return TxSetUtils::hashTxSorter(a.front(),
                                                                    b.front());
                                }))
            {
                CLOG_DEBUG(Herder, "Got bad generalized txSet: "
                                   "stage is not sorted");
                return std::nullopt;
            }
        }
        if (!std::is_sorted(stages.begin(), stages.end(),
                            [](auto const& a, auto const& b) {
                                releaseAssert(!a.empty() && !b.empty());
                                return TxSetUtils::hashTxSorter(
                                    a.front().front(), b.front().front());
                            }))
        {
            CLOG_DEBUG(Herder, "Got bad generalized txSet: "
                               "stages are not sorted");
            return std::nullopt;
        }
        phaseFrame.emplace(
            TxSetPhaseFrame(phase, std::move(stages), inclusionFeeMapPtr));
        break;
    }
    default:
        releaseAssert(false);
    }
    releaseAssert(phaseFrame);
    return phaseFrame;
}

std::optional<TxSetPhaseFrame>
TxSetPhaseFrame::makeFromWireLegacy(
    LedgerHeader const& lclHeader, Hash const& networkID,
    xdr::xvector<TransactionEnvelope> const& xdrTxs)
{
    TxFrameList txList;
    if (!addWireTxsToList(networkID, xdrTxs, txList))
    {
        CLOG_DEBUG(
            Herder,
            "Got bad legacy txSet: transactions are not ordered correctly "
            "or contain invalid transactions");
        return std::nullopt;
    }
    auto inclusionFeeMapPtr = std::make_shared<InclusionFeeMap>();
    auto& inclusionFeeMap = *inclusionFeeMapPtr;
    int64_t baseFee = computeBaseFeeForLegacyTxSet(lclHeader, txList);
    for (auto const& tx : txList)
    {
        inclusionFeeMap[tx] = baseFee;
    }
    return TxSetPhaseFrame(TxSetPhase::CLASSIC, std::move(txList),
                           inclusionFeeMapPtr);
}

TxSetPhaseFrame
TxSetPhaseFrame::makeEmpty(TxSetPhase phase, bool isParallel)
{
    if (isParallel)
    {
        return TxSetPhaseFrame(phase, TxStageFrameList{},
                               std::make_shared<InclusionFeeMap>());
    }
    return TxSetPhaseFrame(phase, TxFrameList{},
                           std::make_shared<InclusionFeeMap>());
}

TxSetPhaseFrame::TxSetPhaseFrame(
    TxSetPhase phase, TxFrameList const& txs,
    std::shared_ptr<InclusionFeeMap> inclusionFeeMap)
    : mPhase(phase), mInclusionFeeMap(inclusionFeeMap), mIsParallel(false)
{
    if (!txs.empty())
    {
        mStages.emplace_back().push_back(txs);
    }
}

TxSetPhaseFrame::TxSetPhaseFrame(
    TxSetPhase phase, TxStageFrameList&& txs,
    std::shared_ptr<InclusionFeeMap> inclusionFeeMap)
    : mPhase(phase)
    , mStages(txs)
    , mInclusionFeeMap(inclusionFeeMap)
    , mIsParallel(true)
{
}

TxSetPhaseFrame::Iterator
TxSetPhaseFrame::begin() const
{
    return TxSetPhaseFrame::Iterator(mStages, 0);
}

TxSetPhaseFrame::Iterator
TxSetPhaseFrame::end() const
{
    return TxSetPhaseFrame::Iterator(mStages, mStages.size());
}

size_t
TxSetPhaseFrame::sizeTx() const
{
    ZoneScoped;
    return std::distance(this->begin(), this->end());
}

size_t
TxSetPhaseFrame::sizeOp() const
{
    ZoneScoped;
    return std::accumulate(this->begin(), this->end(), size_t(0),
                           [&](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

size_t
TxSetPhaseFrame::size(LedgerHeader const& lclHeader) const
{
    switch (mPhase)
    {
    case TxSetPhase::CLASSIC:
        return protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                         ProtocolVersion::V_11)
                   ? sizeOp()
                   : sizeTx();
    case TxSetPhase::SOROBAN:
        return sizeOp();
    default:
        throw std::runtime_error("Unknown phase");
    }
}

bool
TxSetPhaseFrame::empty() const
{
    return sizeTx() == 0;
}

bool
TxSetPhaseFrame::isParallel() const
{
    return mIsParallel;
}

TxStageFrameList const&
TxSetPhaseFrame::getParallelStages() const
{
    releaseAssert(isParallel());
    return mStages;
}

TxFrameList const&
TxSetPhaseFrame::getSequentialTxs() const
{
    releaseAssert(!isParallel());
    static TxFrameList empty;
    if (mStages.empty())
    {
        return empty;
    }
    return mStages.at(0).at(0);
}

void
TxSetPhaseFrame::toXDR(TransactionPhase& xdrPhase) const
{

    if (isParallel())
    {
        parallelPhaseToXdr(mStages, *mInclusionFeeMap, xdrPhase);
    }
    else
    {
        sequentialPhaseToXdr(getSequentialTxs(), *mInclusionFeeMap, xdrPhase);
    }
}

InclusionFeeMap const&
TxSetPhaseFrame::getInclusionFeeMap() const
{
    return *mInclusionFeeMap;
}

TxSetPhaseFrame
TxSetPhaseFrame::sortedForApply(Hash const& txSetHash) const
{
    if (isParallel())
    {
        return TxSetPhaseFrame(mPhase,
                               sortedForApplyParallel(mStages, txSetHash),
                               mInclusionFeeMap);
    }
    else
    {
        return TxSetPhaseFrame(
            mPhase, sortedForApplySequential(getSequentialTxs(), txSetHash),
            mInclusionFeeMap);
    }
}

bool
txValidityDependsOnCloseTime(TransactionEnvelope const& env)
{
    auto timeBounded = [](TimeBounds const& tb) {
        return tb.minTime != 0 || tb.maxTime != 0;
    };
    auto conditional = [&](Preconditions const& cond) {
        switch (cond.type())
        {
        case PRECOND_NONE:
            return false;
        case PRECOND_TIME:
            return timeBounded(cond.timeBounds());
        case PRECOND_V2:
            return (cond.v2().timeBounds &&
                    timeBounded(*cond.v2().timeBounds)) ||
                   cond.v2().minSeqAge != 0;
        }
        // An unknown kind of precondition: assume the worst.
        return true;
    };
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return env.v0().tx.timeBounds && timeBounded(*env.v0().tx.timeBounds);
    case ENVELOPE_TYPE_TX:
        return conditional(env.v1().tx.cond);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return conditional(env.feeBump().tx.innerTx.v1().tx.cond);
    default:
        return true;
    }
}

TxSetValidationContext
TxSetValidationContext::forLCL(Application& app)
{
    releaseAssert(threadIsMain());
    auto& lm = app.getLedgerManager();
    auto const& lcl = lm.getLastClosedLedgerHeader();
    SorobanNetworkConfig const* sorobanConfig = nullptr;
    if (protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        sorobanConfig = &lm.getLastClosedSorobanNetworkConfig();
    }
    size_t maxTasks = std::numeric_limits<size_t>::max();
    // In some cases we might process a transaction set for ledger N while
    // applying ledger N, in which case we won't be able to re-use the batch
    // executor and thus need to fall back to single-threaded execution.
    // This is really an edge case: it requires several tx sets to exist for the
    // same ledger (which is rare), and the timing must be very specific for
    // processing to happen after the vote and application start. Thus in
    // practice we may consider this rare enough to not worry about the
    // performance impact.
    // NB: Both this method and `isApplying` must happen on the main thread,
    // so there is no race condition risk.
    // The same goes for a ledger that the apply thread is executing
    // speculatively (e.g. a later round's leader building its set while the
    // previous round's value is still being executed).
    if (lm.isApplying() || lm.isSpeculativelyApplying())
    {
        maxTasks = 1;
    }
#ifdef BUILD_TESTS
    // In in-memory mode we use a raw LTX in validation, which is not safe to
    // share across multiple threads. That's avoidable, but requires changes to
    // LTX and validation logic, so it's not worth for fixing this just for
    // tests.
    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        maxTasks = 1;
    }
#endif
    return TxSetValidationContext{
        lcl, sorobanConfig, [&lm]() { return lm.getLCLView(); }, maxTasks};
}

bool
TxSetPhaseFrame::checkValid(Application& app,
                            uint64_t lowerBoundCloseTimeOffset,
                            uint64_t upperBoundCloseTimeOffset,
                            bool txsAreValidated) const
{
    UnorderedMap<AccountID, int64_t> accountFeeMap;
    return checkValidWithResult(app, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset, txsAreValidated,
                                accountFeeMap) == TxSetValidationResult::VALID;
}

TxSetValidationResult
TxSetPhaseFrame::checkValidWithResult(
    Application& app, uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset, bool txsAreValidated,
    UnorderedMap<AccountID, int64_t>& accountFeeMap) const
{
    return checkValidWithResult(
        app, TxSetValidationContext::forLCL(app), lowerBoundCloseTimeOffset,
        upperBoundCloseTimeOffset, txsAreValidated, accountFeeMap);
}

TxSetValidationResult
TxSetPhaseFrame::checkValidWithResult(
    Application& app, TxSetValidationContext const& ctx,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    bool txsAreValidated, UnorderedMap<AccountID, int64_t>& accountFeeMap) const
{
    auto const& lcl = ctx.mLcl;
    // Verify the fee map for the phase. This check is independent of the phase
    // type or contents.
    auto feeMapResult = checkFeeMap(getInclusionFeeMap(), lcl.header);
    if (feeMapResult != TxSetValidationResult::VALID)
    {
        return feeMapResult;
    }

    bool isSoroban = mPhase == TxSetPhase::SOROBAN;

    // Ensure that the phase contains only the transactions of expected
    // kind (Soroban or classic).
    for (auto const& tx : *this)
    {
        if (tx->isSoroban() != isSoroban)
        {
            CLOG_DEBUG(Herder,
                       "Got bad generalized txSet with invalid "
                       "phase {} transactions",
                       static_cast<size_t>(mPhase));
            return TxSetValidationResult::INVALID_PHASE_TX_TYPE;
        }
    }

    // Then check the phase-specific properties. This may rely on transactions
    // belonging to the valid phase.
    if (isSoroban && ctx.mSorobanConfig == nullptr)
    {
        // A Soroban phase can't be valid on top of a ledger that has no
        // Soroban configuration.
        CLOG_DEBUG(Herder, "Got bad txSet: Soroban phase before Soroban");
        return TxSetValidationResult::INVALID_PHASE_TX_TYPE;
    }
    auto checkPhaseSpecific =
        isSoroban ? checkValidSoroban(lcl.header, *ctx.mSorobanConfig)
                  : checkValidClassic(lcl.header);
    if (checkPhaseSpecific != TxSetValidationResult::VALID)
    {
        return checkPhaseSpecific;
    }

    if (txsAreValidated)
    {
        return TxSetValidationResult::VALID;
    }

    auto invalid = TxSetUtils::getInvalidTxListWithErrors(
        *this, app, ctx, accountFeeMap, lowerBoundCloseTimeOffset,
        upperBoundCloseTimeOffset);
    if (invalid.first.empty())
    {
        releaseAssert(invalid.second == TxSetValidationResult::VALID);
    }
    return invalid.second;
}

TxSetValidationResult
TxSetPhaseFrame::checkValidClassic(LedgerHeader const& lclHeader) const
{
    if (isParallel())
    {
        CLOG_DEBUG(Herder, "Got bad txSet: classic phase can't be parallel");
        return TxSetValidationResult::CLASSIC_PHASE_PARALLEL_NOT_ALLOWED;
    }
    if (this->size(lclHeader) > lclHeader.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many classic txs {} > {}",
                   this->size(lclHeader), lclHeader.maxTxSetSize);
        return TxSetValidationResult::TOO_MANY_CLASSIC_TXS;
    }
    return TxSetValidationResult::VALID;
}

TxSetValidationResult
TxSetPhaseFrame::checkValidSoroban(
    LedgerHeader const& lclHeader,
    SorobanNetworkConfig const& sorobanConfig) const
{
    bool needParallelSorobanPhase = protocolVersionStartsFrom(
        lclHeader.ledgerVersion, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
    if (isParallel() != needParallelSorobanPhase)
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet: Soroban phase parallel support "
                   "does not match the current protocol; '{}' was "
                   "expected",
                   needParallelSorobanPhase);
        return TxSetValidationResult::SOROBAN_PARALLEL_SUPPORT_MISMATCH;
    }
    // Ensure the total resources are not over ledger limit.
    auto totalResources = getTotalResources(lclHeader.ledgerVersion);
    if (!totalResources)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: total Soroban resources overflow");
        return TxSetValidationResult::SOROBAN_RESOURCES_OVERFLOW;
    }

    auto maxResources = sorobanConfig.maxLedgerResources();

    // With parallel Soroban phase the instruction limit validation is more
    // complex than just comparing the total instructions to the ledger-wide
    // limit. Thus, we skip the instruction check for the parallel phase and
    // do the proper check further below.
    if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        maxResources.setVal(Resource::Type::INSTRUCTIONS,
                            std::numeric_limits<int64_t>::max());
    }
    if (anyGreater(*totalResources, maxResources))
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet: needed resources exceed ledger "
                   "limits {} > {}",
                   totalResources->toString(), maxResources.toString());
        return TxSetValidationResult::SOROBAN_RESOURCES_EXCEED_LIMIT;
    }

    if (!isParallel())
    {
        return TxSetValidationResult::VALID;
    }
    auto const& stages = getParallelStages();

    // Verify that number of clusters is not exceeded per stage. There is no
    // limit for the number of stages or transactions per cluster.
    for (auto const& stage : stages)
    {
        if (stage.size() > sorobanConfig.ledgerMaxDependentTxClusters())
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: too many clusters in Soroban "
                       "stage {} > {}",
                       stage.size(),
                       sorobanConfig.ledgerMaxDependentTxClusters());
            return TxSetValidationResult::TOO_MANY_SOROBAN_CLUSTERS;
        }
    }

    // Verify that 'sequential' instructions don't exceed the ledger-wide
    // limit.
    // Every may have multiple clusters and its runtime is considered to be
    // bounded by the slowest cluster (i.e. the one with the most instructions).
    // Stages are meant to be executed sequentially, so the ledger-wide
    // instructions should be limited by the sum of the stages' instructions.
    int64_t totalInstructions = 0;
    for (auto const& stage : stages)
    {
        int64_t stageInstructions = 0;
        for (auto const& cluster : stage)
        {
            int64_t clusterInstructions = 0;
            for (auto const& tx : cluster)
            {
                // clusterInstructions + tx->sorobanResources().instructions >
                //     std::numeric_limits<int64_t>::max()
                if (clusterInstructions >
                    std::numeric_limits<int64_t>::max() -
                        tx->sorobanResources().instructions)
                {
                    CLOG_DEBUG(Herder, "Got bad txSet: Soroban sequential "
                                       "instructions overflow");
                    return TxSetValidationResult::
                        SOROBAN_SEQUENTIAL_INSTRUCTIONS_OVERFLOW;
                }
                clusterInstructions += tx->sorobanResources().instructions;
            }
            stageInstructions =
                std::max(stageInstructions, clusterInstructions);
        }
        // totalInstructions + stageInstructions >
        //     std::numeric_limits<int64_t>::max()
        if (totalInstructions >
            std::numeric_limits<int64_t>::max() - stageInstructions)
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: Soroban total instructions overflow");
            return TxSetValidationResult::SOROBAN_INSTRUCTIONS_OVERFLOW;
        }
        totalInstructions += stageInstructions;
    }
    if (totalInstructions > sorobanConfig.ledgerMaxInstructions())
    {
        CLOG_DEBUG(
            Herder,
            "Got bad txSet: Soroban total instructions exceed limit: {} > {}",
            totalInstructions, sorobanConfig.ledgerMaxInstructions());
        return TxSetValidationResult::SOROBAN_INSTRUCTIONS_EXCEED_LIMIT;
    }

    // Verify that there are no read-write conflicts between clusters within
    // every stage.
    for (auto const& stage : stages)
    {
        UnorderedSet<LedgerKey> stageReadOnlyKeys;
        UnorderedSet<LedgerKey> stageReadWriteKeys;
        for (auto const& cluster : stage)
        {
            std::vector<LedgerKey> clusterReadOnlyKeys;
            std::vector<LedgerKey> clusterReadWriteKeys;
            for (auto const& tx : cluster)
            {
                auto const& footprint = tx->sorobanResources().footprint;

                for (auto const& key : footprint.readOnly)
                {
                    if (stageReadWriteKeys.count(key) > 0)
                    {
                        CLOG_DEBUG(
                            Herder,
                            "Got bad generalized txSet: cluster footprint "
                            "conflicts with another cluster within stage");
                        return TxSetValidationResult::TX_ORDERING_INVALID;
                    }
                    clusterReadOnlyKeys.push_back(key);
                }
                for (auto const& key : footprint.readWrite)
                {
                    if (stageReadOnlyKeys.count(key) > 0 ||
                        stageReadWriteKeys.count(key) > 0)
                    {
                        CLOG_DEBUG(
                            Herder,
                            "Got bad generalized txSet: cluster footprint "
                            "conflicts with another cluster within stage");
                        return TxSetValidationResult::TX_ORDERING_INVALID;
                    }
                    clusterReadWriteKeys.push_back(key);
                }
            }
            stageReadOnlyKeys.insert(clusterReadOnlyKeys.begin(),
                                     clusterReadOnlyKeys.end());
            stageReadWriteKeys.insert(clusterReadWriteKeys.begin(),
                                      clusterReadWriteKeys.end());
        }
    }
    return TxSetValidationResult::VALID;
}

std::optional<Resource>
TxSetPhaseFrame::getTotalResources(uint32_t ledgerVersion) const
{
    auto total = mPhase == TxSetPhase::SOROBAN ? Resource::makeEmptySoroban()
                                               : Resource::makeEmpty(1);
    for (auto const& tx : *this)
    {
        if (total.canAdd(tx->getResources(/* useByteLimitInClassic */ false,
                                          ledgerVersion)))
        {
            total += tx->getResources(/* useByteLimitInClassic */ false,
                                      ledgerVersion);
        }
        else
        {
            return std::nullopt;
        }
    }
    return std::make_optional<Resource>(total);
}

ApplicableTxSetFrame::ApplicableTxSetFrame(
    Application& app, bool isGeneralized, Hash const& previousLedgerHash,
    std::vector<TxSetPhaseFrame> const& phases,
    std::optional<Hash> contentsHash)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mPhases(phases)
    , mContentsHash(contentsHash)
{
    // When applying in the background, the same check is performed in
    // closeLedger already
    if (threadIsMain())
    {
        releaseAssert(previousLedgerHash ==
                      app.getLedgerManager().getLastClosedLedgerHeader().hash);
    }
}

ApplicableTxSetFrame::ApplicableTxSetFrame(
    Application& app, LedgerHeaderHistoryEntry const& lclHeader,
    std::vector<TxSetPhaseFrame> const& phases,
    std::optional<Hash> contentsHash)
    : ApplicableTxSetFrame(
          app,
          protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                    SOROBAN_PROTOCOL_VERSION),
          lclHeader.hash, phases, contentsHash)
{
}

Hash const&
ApplicableTxSetFrame::getContentsHash() const
{
    releaseAssert(mContentsHash);
    return *mContentsHash;
}

TxSetPhaseFrame const&
ApplicableTxSetFrame::getPhase(TxSetPhase phaseTxs) const
{
    releaseAssert(static_cast<size_t>(phaseTxs) < mPhases.size());
    return mPhases.at(static_cast<size_t>(phaseTxs));
}

std::vector<TxSetPhaseFrame> const&
ApplicableTxSetFrame::getPhases() const
{
    return mPhases;
}

std::vector<TxSetPhaseFrame> const&
ApplicableTxSetFrame::getPhasesInApplyOrder() const
{
    ZoneScoped;
    if (mApplyOrderPhases.empty())
    {
        mApplyOrderPhases.reserve(mPhases.size());
        for (auto const& phaseTxs : mPhases)
        {
            mApplyOrderPhases.emplace_back(
                phaseTxs.sortedForApply(getContentsHash()));
        }
    }
    return mApplyOrderPhases;
}

bool
ApplicableTxSetFrame::checkValid(Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    return checkValidWithResult(app, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset) ==
           TxSetValidationResult::VALID;
}

TxSetValidationResult
ApplicableTxSetFrame::checkValidWithResult(
    Application& app, uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset) const
{
    // For public-facing methods, always do full validation
    return checkValidInternalWithResult(app, lowerBoundCloseTimeOffset,
                                        upperBoundCloseTimeOffset,
                                        /* txsAreValidated */ false);
}

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
ApplicableTxSetFrame::checkValidInternal(Application& app,
                                         uint64_t lowerBoundCloseTimeOffset,
                                         uint64_t upperBoundCloseTimeOffset,
                                         bool txsAreValidated) const
{
    return checkValidInternalWithResult(
               app, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
               txsAreValidated) == TxSetValidationResult::VALID;
}

TxSetValidationResult
ApplicableTxSetFrame::checkValidWithResult(
    Application& app, TxSetValidationContext const& ctx,
    uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset) const
{
    return checkValidInternalWithResult(app, ctx, lowerBoundCloseTimeOffset,
                                        upperBoundCloseTimeOffset,
                                        /* txsAreValidated */ false);
}

TxSetValidationResult
ApplicableTxSetFrame::checkValidInternalWithResult(
    Application& app, uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset, bool txsAreValidated) const
{
    releaseAssert(threadIsMain());
    return checkValidInternalWithResult(
        app, TxSetValidationContext::forLCL(app), lowerBoundCloseTimeOffset,
        upperBoundCloseTimeOffset, txsAreValidated);
}

TxSetValidationResult
ApplicableTxSetFrame::checkValidInternalWithResult(
    Application& app, TxSetValidationContext const& ctx,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    bool txsAreValidated) const
{
    ZoneScoped;
    auto const& lcl = ctx.mLcl;

    // Start by checking previousLedgerHash
    if (lcl.hash != mPreviousLedgerHash)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {}, expected {}",
                   hexAbbrev(mPreviousLedgerHash), hexAbbrev(lcl.hash));
        return TxSetValidationResult::PREVIOUS_LEDGER_HASH_MISMATCH;
    }

    bool needGeneralizedTxSet = protocolVersionStartsFrom(
        lcl.header.ledgerVersion, SOROBAN_PROTOCOL_VERSION);
    if (needGeneralizedTxSet != isGeneralizedTxSet())
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet {}: need generalized '{}', expected '{}'",
                   hexAbbrev(mPreviousLedgerHash), needGeneralizedTxSet,
                   isGeneralizedTxSet());
        return TxSetValidationResult::GENERALIZED_TXSET_MISMATCH;
    }

    if (isGeneralizedTxSet())
    {
        // Generalized transaction sets should always have 2 phases by
        // construction.
        releaseAssert(mPhases.size() ==
                      static_cast<size_t>(TxSetPhase::PHASE_COUNT));
    }
    else
    {
        // Legacy tx sets should have 1 phase by construction.
        releaseAssert(mPhases.size() == 1);
    }

    return checkPhasesValid(app, ctx, mPhases, isGeneralizedTxSet(),
                            lowerBoundCloseTimeOffset,
                            upperBoundCloseTimeOffset, txsAreValidated);
}

TxSetValidationResult
ApplicableTxSetFrame::checkPhasesValid(
    Application& app, TxSetValidationContext const& ctx,
    std::vector<TxSetPhaseFrame> const& phases, bool isGeneralized,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    bool txsAreValidated)
{
    auto const& lcl = ctx.mLcl;
    bool enforceUniqueSourceAccount = isGeneralized;
#ifdef BUILD_TESTS
    if (app.getRunInOverlayOnlyMode())
    {
        enforceUniqueSourceAccount = false;
    }
#endif
    if (enforceUniqueSourceAccount)
    {
        // Ensure the tx set does not contain multiple txs per source
        // account
        std::unordered_set<AccountID> seenAccounts;
        for (auto const& phase : phases)
        {
            for (auto const& tx : phase)
            {
                if (!seenAccounts.insert(tx->getSourceID()).second)
                {
                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: multiple txs per source account");
                    return TxSetValidationResult::
                        MULTIPLE_TXS_PER_SOURCE_ACCOUNT;
                }
            }
        }
    }

    bool useCrossPhaseFeeMap = protocolVersionStartsFrom(
        lcl.header.ledgerVersion, ProtocolVersion::V_26);
    UnorderedMap<AccountID, int64_t> accountFeeMap;
    for (auto const& phase : phases)
    {
        if (!useCrossPhaseFeeMap)
        {
            accountFeeMap.clear();
        }
        auto result = phase.checkValidWithResult(
            app, ctx, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
            txsAreValidated, accountFeeMap);
        if (result != TxSetValidationResult::VALID)
        {
            return result;
        }
    }

    return TxSetValidationResult::VALID;
}

size_t
ApplicableTxSetFrame::size(LedgerHeader const& lh,
                           std::optional<TxSetPhase> phaseType) const
{
    ZoneScoped;
    if (phaseType)
    {
        return mPhases.at(static_cast<size_t>(*phaseType)).size(lh);
    }

    size_t sz = 0;
    for (auto const& phase : mPhases)
    {
        sz += phase.size(lh);
    }
    return sz;
}

size_t
ApplicableTxSetFrame::sizeOp(TxSetPhase phase) const
{
    return mPhases.at(static_cast<size_t>(phase)).sizeOp();
}

size_t
ApplicableTxSetFrame::sizeOpTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (auto const& phase : mPhases)
    {
        total += phase.sizeOp();
    }
    return total;
}

size_t
ApplicableTxSetFrame::sizeTx(TxSetPhase phase) const
{
    return mPhases.at(static_cast<size_t>(phase)).sizeTx();
}

size_t
ApplicableTxSetFrame::sizeTxTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (auto const& phase : mPhases)
    {
        total += phase.sizeTx();
    }
    return total;
}

std::optional<int64_t>
ApplicableTxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx) const
{
    for (auto const& phase : mPhases)
    {
        auto const& phaseMap = phase.getInclusionFeeMap();
        if (auto it = phaseMap.find(tx); it != phaseMap.end())
        {
            return it->second;
        }
    }
    throw std::runtime_error("Transaction not found in tx set");
}

int64_t
ApplicableTxSetFrame::getTotalFees(LedgerHeader const& lh) const
{
    ZoneScoped;
    int64_t total{0};
    for (auto const& phaseTxs : mPhases)
    {
        for (auto const& tx : phaseTxs)
        {
            total =
                saturatingAdd(total, tx->getFee(lh, getTxBaseFee(tx), true));
        }
    }
    return total;
}

int64_t
ApplicableTxSetFrame::getTotalInclusionFees() const
{
    ZoneScoped;
    int64_t total{0};
    for (auto const& phaseTxs : mPhases)
    {
        for (auto const& tx : phaseTxs)
        {
            total = saturatingAdd(total, tx->getInclusionFee());
        }
    }
    return total;
}

std::string
ApplicableTxSetFrame::summary() const
{
    if (empty())
    {
        return "empty tx set";
    }
    if (!isGeneralizedTxSet())
    {
        return fmt::format(
            FMT_STRING("txs:{}, ops:{}, base_fee:{}"), sizeTxTotal(),
            sizeOpTotal(),
            // NB: fee map can't be empty at this stage (checked above).
            mPhases[static_cast<size_t>(TxSetPhase::CLASSIC)]
                .getInclusionFeeMap()
                .begin()
                ->second.value_or(0));
    }

    auto feeStats = [&](auto const& feeMap) {
        std::map<std::optional<int64_t>, std::pair<int, int>> componentStats;
        for (auto const& [tx, fee] : feeMap)
        {
            ++componentStats[fee].first;
            componentStats[fee].second += tx->getNumOperations();
        }
        std::string res = fmt::format(FMT_STRING("{} component(s): ["),
                                      componentStats.size());

        for (auto const& [fee, stats] : componentStats)
        {
            if (fee != componentStats.begin()->first)
            {
                res += ", ";
            }
            if (fee)
            {
                res += fmt::format(
                    FMT_STRING("{{discounted txs:{}, ops:{}, base_fee:{}}}"),
                    stats.first, stats.second, *fee);
            }
            else
            {
                res +=
                    fmt::format(FMT_STRING("{{non-discounted txs:{}, ops:{}}}"),
                                stats.first, stats.second);
            }
        }
        res += "]";
        return res;
    };

    std::string status;
    releaseAssert(mPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));
    for (size_t i = 0; i < mPhases.size(); i++)
    {
        if (!status.empty())
        {
            status += ", ";
        }
        status += fmt::format(FMT_STRING("{} phase: {}"),
                              getTxSetPhaseName(static_cast<TxSetPhase>(i)),
                              feeStats(mPhases[i].getInclusionFeeMap()));
    }
    return status;
}

void
ApplicableTxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    releaseAssert(mPhases.size() == 1);
    transactionsToTransactionSetXDR(mPhases[0].getSequentialTxs(),
                                    mPreviousLedgerHash, txSet);
}

void
ApplicableTxSetFrame::toXDR(GeneralizedTransactionSet& generalizedTxSet) const
{
    ZoneScoped;
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(mPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));
    transactionsToGeneralizedTransactionSetXDR(mPhases, mPreviousLedgerHash,
                                               generalizedTxSet);
}

TxSetXDRFrameConstPtr
ApplicableTxSetFrame::toWireTxSetFrame() const
{
    TxSetXDRFrameConstPtr outputTxSet;
    if (mIsGeneralized)
    {
        GeneralizedTransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        outputTxSet = TxSetXDRFrame::makeFromWire(std::move(xdrTxSet));
    }
    else
    {
        TransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        outputTxSet = TxSetXDRFrame::makeFromWire(std::move(xdrTxSet));
    }
    return outputTxSet;
}

bool
ApplicableTxSetFrame::isGeneralizedTxSet() const
{
    return mIsGeneralized;
}

std::string
toString(TxSetValidationResult result)
{
    switch (result)
    {
    case TxSetValidationResult::VALID:
        return "VALID";
    case TxSetValidationResult::INCORRECT_COMPONENT_ORDER:
        return "INCORRECT_COMPONENT_ORDER";
    case TxSetValidationResult::DUPLICATE_COMPONENT_BASE_FEES:
        return "DUPLICATE_COMPONENT_BASE_FEES";
    case TxSetValidationResult::EMPTY_COMPONENT:
        return "EMPTY_COMPONENT";
    case TxSetValidationResult::EMPTY_STAGE:
        return "EMPTY_STAGE";
    case TxSetValidationResult::EMPTY_CLUSTER:
        return "EMPTY_CLUSTER";
    case TxSetValidationResult::UNSUPPORTED_VERSION:
        return "UNSUPPORTED_VERSION";
    case TxSetValidationResult::UNSUPPORTED_PHASE_VERSION:
        return "UNSUPPORTED_PHASE_VERSION";
    case TxSetValidationResult::NON_SOROBAN_PARALLEL_PHASE:
        return "NON_SOROBAN_PARALLEL_PHASE";
    case TxSetValidationResult::WRONG_PHASE_COUNT:
        return "WRONG_PHASE_COUNT";
    case TxSetValidationResult::PREVIOUS_LEDGER_HASH_MISMATCH:
        return "PREVIOUS_LEDGER_HASH_MISMATCH";
    case TxSetValidationResult::GENERALIZED_TXSET_MISMATCH:
        return "GENERALIZED_TXSET_MISMATCH";
    case TxSetValidationResult::CLASSIC_PHASE_PARALLEL_NOT_ALLOWED:
        return "CLASSIC_PHASE_PARALLEL_NOT_ALLOWED";
    case TxSetValidationResult::TOO_MANY_CLASSIC_TXS:
        return "TOO_MANY_CLASSIC_TXS";
    case TxSetValidationResult::SOROBAN_PARALLEL_SUPPORT_MISMATCH:
        return "SOROBAN_PARALLEL_SUPPORT_MISMATCH";
    case TxSetValidationResult::SOROBAN_RESOURCES_OVERFLOW:
        return "SOROBAN_RESOURCES_OVERFLOW";
    case TxSetValidationResult::SOROBAN_RESOURCES_EXCEED_LIMIT:
        return "SOROBAN_RESOURCES_EXCEED_LIMIT";
    case TxSetValidationResult::TOO_MANY_SOROBAN_CLUSTERS:
        return "TOO_MANY_SOROBAN_CLUSTERS";
    case TxSetValidationResult::SOROBAN_INSTRUCTIONS_OVERFLOW:
        return "SOROBAN_INSTRUCTIONS_OVERFLOW";
    case TxSetValidationResult::SOROBAN_INSTRUCTIONS_EXCEED_LIMIT:
        return "SOROBAN_INSTRUCTIONS_EXCEED_LIMIT";
    case TxSetValidationResult::SOROBAN_SEQUENTIAL_INSTRUCTIONS_OVERFLOW:
        return "SOROBAN_SEQUENTIAL_INSTRUCTIONS_OVERFLOW";
    case TxSetValidationResult::MULTIPLE_TXS_PER_SOURCE_ACCOUNT:
        return "MULTIPLE_TXS_PER_SOURCE_ACCOUNT";
    case TxSetValidationResult::INVALID_PHASE_TX_TYPE:
        return "INVALID_PHASE_TX_TYPE";
    case TxSetValidationResult::TX_ORDERING_INVALID:
        return "TX_ORDERING_INVALID";
    case TxSetValidationResult::COMPONENT_BASE_FEE_TOO_LOW:
        return "COMPONENT_BASE_FEE_TOO_LOW";
    case TxSetValidationResult::TX_FEE_BID_TOO_LOW:
        return "TX_FEE_BID_TOO_LOW";
    case TxSetValidationResult::TX_VALIDATION_FAILED:
        return "TX_VALIDATION_FAILED";
    case TxSetValidationResult::ACCOUNT_CANT_PAY_FEE:
        return "ACCOUNT_CANT_PAY_FEE";
    default:
        return "UNKNOWN";
    }
}

} // namespace stellar
