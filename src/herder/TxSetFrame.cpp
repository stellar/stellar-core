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
#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/Peer.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <algorithm>
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

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
bool
validateParallelComponent(ParallelTxsComponent const& component)
{
    for (auto const& stage : component.executionStages)
    {
        if (stage.empty())
        {
            CLOG_DEBUG(Herder, "Got bad txSet: empty stage");
            return false;
        }
        for (auto const& thread : stage)
        {
            if (thread.empty())
            {
                CLOG_DEBUG(Herder, "Got bad txSet: empty thread");
                return false;
            }
        }
    }
    return true;
}
#endif

bool
validateTxSetXDRStructure(GeneralizedTransactionSet const& txSet)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    int const MAX_PHASE = 1;
#else
    int const MAX_PHASE = 0;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        if (phase.v() == 1)
        {
            if (phaseId != static_cast<size_t>(TxSetPhase::SOROBAN))
            {
                CLOG_DEBUG(Herder,
                           "Got bad txSet: non-Soroban parallel phase {}",
                           phase.v());
                return false;
            }
            if (!validateParallelComponent(phase.parallelTxsComponent()))
            {
                return false;
            }
        }
        else
#endif
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
    auto txOps = tx.getNumOperations();
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

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
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
        for (auto const& thread : stage)
        {
            auto& xdrThread = xdrStage.emplace_back();
            xdrThread.reserve(thread.size());
            for (auto const& tx : thread)
            {
                xdrThread.push_back(tx->getEnvelope());
            }
        }
    }
}

#endif

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
        for (auto& thread : stage)
        {
            std::sort(thread.begin(), thread.end(), sorter);
        }
        // There is no need to shuffle threads in the stage, as they are
        // independent, so the apply order doesn't matter even if the threads
        // are being applied sequentially.
    }
    std::sort(sortedStages.begin(), sortedStages.end(),
              [&sorter](auto const& a, auto const& b) {
                  releaseAssert(!a.empty() && !b.empty());
                  releaseAssert(!a.front().empty() && !b.front().empty());
                  return sorter(a.front().front(), b.front().front());
              });
    return stages;
}

// This assumes that the phase validation has already been done,
// specifically that there are no transactions that belong to the same
// source account, and that the ledger sequence corresponds to the
bool
phaseTxsAreValid(TxSetPhaseFrame const& phase, Application& app,
                 uint64_t lowerBoundCloseTimeOffset,
                 uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    // This is done so minSeqLedgerGap is validated against the next
    // ledgerSeq, which is what will be used at apply time

    // Grab read-only latest ledger state; This is only used to validate tx sets
    // for LCL+1
    LedgerSnapshot ls(app);
    ls.getLedgerHeader().currentToModify().ledgerSeq += 1;
    // TODO: Double check that the readonly soroban network config is what we
    // want here.
    AppValidationWrapper const avw(app.getAppConnector(), false, std::nullopt);
    for (auto const& tx : phase)
    {
        auto txResult = tx->checkValid(avw, ls, 0, lowerBoundCloseTimeOffset,
                                       upperBoundCloseTimeOffset);
        if (!txResult->isSuccess())
        {

            CLOG_DEBUG(
                Herder, "Got bad txSet: tx invalid tx: {} result: {}",
                xdrToCerealString(tx->getEnvelope(), "TransactionEnvelope"),
                txResult->getResultCode());
            return false;
        }
    }
    return true;
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
        if (!tx->XDRProvidesValidFee())
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

std::pair<TxFrameList, std::shared_ptr<InclusionFeeMap>>
applySurgePricing(TxSetPhase phase, TxFrameList const& txs, Application& app)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    releaseAssert(!app.getLedgerManager().isApplying());

    auto const& lclHeader =
        app.getLedgerManager().getLastClosedLedgerHeader().header;
    std::vector<bool> hadTxNotFittingLane;
    std::shared_ptr<SurgePricingLaneConfig> surgePricingLaneConfig;
    if (phase == TxSetPhase::CLASSIC)
    {
        auto maxOps =
            Resource({static_cast<uint32_t>(
                          app.getLedgerManager().getLastMaxTxSetSizeOps()),
                      MAX_CLASSIC_BYTE_ALLOWANCE});
        std::optional<Resource> dexOpsLimit;
        if (app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET)
        {
            // DEX operations limit implies that DEX transactions should
            // compete with each other in in a separate fee lane, which
            // is only possible with generalized tx set.
            dexOpsLimit =
                Resource({*app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET,
                          MAX_CLASSIC_BYTE_ALLOWANCE});
        }

        surgePricingLaneConfig =
            std::make_shared<DexLimitingLaneConfig>(maxOps, dexOpsLimit);
    }
    else
    {
        releaseAssert(phase == TxSetPhase::SOROBAN);

        auto limits = app.getLedgerManager().maxLedgerResources(
            /* isSoroban */ true);

        auto byteLimit =
            std::min(static_cast<int64_t>(MAX_SOROBAN_BYTE_ALLOWANCE),
                     limits.getVal(Resource::Type::TX_BYTE_SIZE));
        limits.setVal(Resource::Type::TX_BYTE_SIZE, byteLimit);

        surgePricingLaneConfig =
            std::make_shared<SorobanGenericLaneConfig>(limits);
    }
    auto includedTxs = SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
        txs, surgePricingLaneConfig, hadTxNotFittingLane);

    size_t laneCount = surgePricingLaneConfig->getLaneLimits().size();
    std::vector<int64_t> lowestLaneFee(laneCount,
                                       std::numeric_limits<int64_t>::max());
    for (auto const& tx : includedTxs)
    {
        size_t lane = surgePricingLaneConfig->getLane(*tx);
        auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
        lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
    }
    auto laneBaseFee =
        computeLaneBaseFee(phase, lclHeader, *surgePricingLaneConfig,
                           lowestLaneFee, hadTxNotFittingLane);
    auto inclusionFeeMapPtr = std::make_shared<InclusionFeeMap>();
    auto& inclusionFeeMap = *inclusionFeeMapPtr;
    for (auto const& tx : includedTxs)
    {
        inclusionFeeMap[tx] = laneBaseFee[surgePricingLaneConfig->getLane(*tx)];
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
makeTxSetFromTransactions(PerPhaseTransactionList const& txPhases,
                          Application& app, uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset
#ifdef BUILD_TESTS
                          ,
                          bool skipValidation
#endif
)
{
    PerPhaseTransactionList invalidTxs;
    invalidTxs.resize(txPhases.size());
    return makeTxSetFromTransactions(txPhases, app, lowerBoundCloseTimeOffset,
                                     upperBoundCloseTimeOffset, invalidTxs
#ifdef BUILD_TESTS
                                     ,
                                     skipValidation
#endif
    );
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(PerPhaseTransactionList const& txPhases,
                          Application& app, uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          PerPhaseTransactionList& invalidTxs
#ifdef BUILD_TESTS
                          ,
                          bool skipValidation
#endif
)
{
    releaseAssert(threadIsMain());
    releaseAssert(!app.getLedgerManager().isApplying());
    releaseAssert(txPhases.size() == invalidTxs.size());
    releaseAssert(txPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));

    std::vector<TxSetPhaseFrame> validatedPhases;
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
        if (skipValidation)
        {
            validatedTxs = phaseTxs;
        }
        else
        {
#endif
            validatedTxs = TxSetUtils::trimInvalid(
                phaseTxs, app, lowerBoundCloseTimeOffset,
                upperBoundCloseTimeOffset, invalid);
#ifdef BUILD_TESTS
        }
#endif
        auto phaseType = static_cast<TxSetPhase>(i);
        auto [includedTxs, inclusionFeeMap] =
            applySurgePricing(phaseType, validatedTxs, app);
        if (phaseType != TxSetPhase::SOROBAN ||
            protocolVersionIsBefore(app.getLedgerManager()
                                        .getLastClosedLedgerHeader()
                                        .header.ledgerVersion,
                                    PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
        {
            validatedPhases.emplace_back(
                TxSetPhaseFrame(std::move(includedTxs), inclusionFeeMap));
        }
        // This is a temporary stub for building a valid parallel tx set
        // without any parallelization.
        else
        {
            TxStageFrameList stages;
            if (!includedTxs.empty())
            {
                stages.emplace_back().push_back(includedTxs);
            }
            validatedPhases.emplace_back(
                TxSetPhaseFrame(std::move(stages), inclusionFeeMap));
        }
    }

    auto const& lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    // Preliminary applicable frame - we don't know the contents hash yet, but
    // we also don't return this.
    std::unique_ptr<ApplicableTxSetFrame> preliminaryApplicableTxSet(
        new ApplicableTxSetFrame(app, lclHeader, validatedPhases,
                                 std::nullopt));

    // Do the roundtrip through XDR to ensure we never build an incorrect tx set
    // for nomination.
    auto outputTxSet = preliminaryApplicableTxSet->toWireTxSetFrame();
#ifdef BUILD_TESTS
    if (skipValidation)
    {
        // Fill in the contents hash if we're skipping the normal roundtrip
        // and validation flow.
        preliminaryApplicableTxSet->mContentsHash =
            outputTxSet->getContentsHash();
        return std::make_pair(outputTxSet,
                              std::move(preliminaryApplicableTxSet));
    }
#endif

    ApplicableTxSetFrameConstPtr outputApplicableTxSet =
        outputTxSet->prepareForApply(app);

    if (!outputApplicableTxSet)
    {
        throw std::runtime_error(
            "Couldn't prepare created tx set frame for apply");
    }

    // Make sure no transactions were lost during the roundtrip and the output
    // tx set is valid.
    bool valid = preliminaryApplicableTxSet->numPhases() ==
                 outputApplicableTxSet->numPhases();
    if (valid)
    {
        for (size_t i = 0; i < preliminaryApplicableTxSet->numPhases(); ++i)
        {
            valid = valid && preliminaryApplicableTxSet->sizeTx(
                                 static_cast<TxSetPhase>(i)) ==
                                 outputApplicableTxSet->sizeTx(
                                     static_cast<TxSetPhase>(i));
        }
    }

    valid = valid &&
            outputApplicableTxSet->checkValid(app, lowerBoundCloseTimeOffset,
                                              upperBoundCloseTimeOffset);
    if (!valid)
    {
        throw std::runtime_error("Created invalid tx set frame");
    }

    return std::make_pair(outputTxSet, std::move(outputApplicableTxSet));
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeEmpty(LedgerHeaderHistoryEntry const& lclHeader)
{
    if (protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        std::vector<TxSetPhaseFrame> emptyPhases(
            static_cast<size_t>(TxSetPhase::PHASE_COUNT),
            TxSetPhaseFrame::makeEmpty(false));
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        if (protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                      PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
        {
            emptyPhases[static_cast<size_t>(TxSetPhase::SOROBAN)] =
                TxSetPhaseFrame::makeEmpty(true);
        }
#endif

        GeneralizedTransactionSet txSet;
        transactionsToGeneralizedTransactionSetXDR(emptyPhases, lclHeader.hash,
                                                   txSet);
        return TxSetXDRFrame::makeFromWire(txSet);
    }
    TransactionSet txSet;
    transactionsToTransactionSetXDR({}, lclHeader.hash, txSet);
    return TxSetXDRFrame::makeFromWire(txSet);
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                           TxFrameList const& txs)
{
    TransactionSet txSet;
    transactionsToTransactionSetXDR(txs, previousLedgerHash, txSet);
    return TxSetXDRFrame::makeFromWire(txSet);
}

#ifdef BUILD_TESTS
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxFrameList txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          bool enforceTxsApplyOrder)
{
    TxFrameList invalid;
    return makeTxSetFromTransactions(txs, app, lowerBoundCloseTimeOffset,
                                     upperBoundCloseTimeOffset, invalid,
                                     enforceTxsApplyOrder);
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxFrameList txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          TxFrameList& invalidTxs, bool enforceTxsApplyOrder)
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
        perPhaseTxs, app, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
        invalid, enforceTxsApplyOrder);
    if (enforceTxsApplyOrder)
    {
        auto const& resPhases = res.second->getPhases();
        // This only supports sequential tx sets for now.
        std::vector<TxSetPhaseFrame> overridePhases;
        for (size_t i = 0; i < resPhases.size(); ++i)
        {
            overridePhases.emplace_back(
                TxSetPhaseFrame(std::move(perPhaseTxs[i]),
                                std::make_shared<InclusionFeeMap>(
                                    resPhases[i].getInclusionFeeMap())));
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
TxSetXDRFrame::prepareForApply(Application& app) const
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
        if (!validateTxSetXDRStructure(xdrTxSet))
        {
            CLOG_DEBUG(Herder,
                       "Got bad generalized txSet with invalid XDR structure");
            return nullptr;
        }
        auto const& xdrPhases = xdrTxSet.v1TxSet().phases;

        for (auto const& xdrPhase : xdrPhases)
        {
            auto maybePhase =
                TxSetPhaseFrame::makeFromWire(app.getNetworkID(), xdrPhase);
            if (!maybePhase)
            {
                return nullptr;
            }
            phaseFrames.emplace_back(std::move(*maybePhase));
        }
        for (size_t phaseId = 0; phaseId < phaseFrames.size(); ++phaseId)
        {
            auto phase = static_cast<TxSetPhase>(phaseId);
            for (auto const& tx : phaseFrames[phaseId])
            {
                if ((tx->isSoroban() && phase != TxSetPhase::SOROBAN) ||
                    (!tx->isSoroban() && phase != TxSetPhase::CLASSIC))
                {
                    CLOG_DEBUG(Herder, "Got bad generalized txSet with invalid "
                                       "phase transactions");
                    return nullptr;
                }
            }
        }
    }
    else
    {
        auto const& xdrTxSet = std::get<TransactionSet>(mXDRTxSet);
        auto maybePhase = TxSetPhaseFrame::makeFromWireLegacy(
            app.getLedgerManager().getLastClosedLedgerHeader().header,
            app.getNetworkID(), xdrTxSet.txs);
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& thread : stage)
                    {
                        totalSize += thread.size();
                    }
                }
                break;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& thread : stage)
                    {
                        totalSize +=
                            std::accumulate(thread.begin(), thread.end(), 0ull,
                                            accumulateTxsFn);
                    }
                }
                break;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            case 1:
                for (auto const& stage :
                     phase.parallelTxsComponent().executionStages)
                {
                    for (auto const& thread : stage)
                    {
                        for (auto const& tx : thread)
                        {
                            txs.emplace_back(
                                TransactionFrameBase::makeTransactionFromWire(
                                    networkID, tx));
                        }
                    }
                }
                break;
#endif
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
        mThreadIndex >= mStages[mStageIndex].size() ||
        mTxIndex >= mStages[mStageIndex][mThreadIndex].size())
    {
        throw std::runtime_error("TxPhase iterator out of bounds");
    }
    return mStages[mStageIndex][mThreadIndex][mTxIndex];
}

TxSetPhaseFrame::Iterator&
TxSetPhaseFrame::Iterator::operator++()
{
    if (mStageIndex >= mStages.size())
    {
        throw std::runtime_error("TxPhase iterator out of bounds");
    }
    ++mTxIndex;
    if (mTxIndex >= mStages[mStageIndex][mThreadIndex].size())
    {
        mTxIndex = 0;
        ++mThreadIndex;
        if (mThreadIndex >= mStages[mStageIndex].size())
        {
            mThreadIndex = 0;
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
           mThreadIndex == other.mThreadIndex && mTxIndex == other.mTxIndex &&
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
TxSetPhaseFrame::makeFromWire(Hash const& networkID,
                              TransactionPhase const& xdrPhase)
{
    auto inclusionFeeMapPtr = std::make_shared<InclusionFeeMap>();
    auto& inclusionFeeMap = *inclusionFeeMapPtr;
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
        return TxSetPhaseFrame(std::move(txList), inclusionFeeMapPtr);
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 1:
    {
        auto const& xdrStages = xdrPhase.parallelTxsComponent().executionStages;
        std::optional<int64_t> baseFee;
        if (xdrPhase.parallelTxsComponent().baseFee)
        {
            baseFee = *xdrPhase.parallelTxsComponent().baseFee;
        }
        TxStageFrameList stages;
        stages.reserve(xdrStages.size());
        for (auto const& xdrStage : xdrStages)
        {
            auto& stage = stages.emplace_back();
            stage.reserve(xdrStage.size());
            for (auto const& xdrThread : xdrStage)
            {
                auto& thread = stage.emplace_back();
                thread.reserve(xdrThread.size());
                for (auto const& env : xdrThread)
                {
                    auto tx = TransactionFrameBase::makeTransactionFromWire(
                        networkID, env);
                    if (!tx->XDRProvidesValidFee())
                    {
                        CLOG_DEBUG(Herder, "Got bad generalized txSet: "
                                           "transaction has invalid XDR");
                        return std::nullopt;
                    }
                    thread.push_back(tx);
                    inclusionFeeMap[tx] = baseFee;
                }
                if (!std::is_sorted(thread.begin(), thread.end(),
                                    &TxSetUtils::hashTxSorter))
                {
                    CLOG_DEBUG(Herder, "Got bad generalized txSet: "
                                       "thread is not sorted");
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
        return TxSetPhaseFrame(std::move(stages), inclusionFeeMapPtr);
    }
#endif
    }

    return std::nullopt;
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
            "or contain invalid phase transactions");
        return std::nullopt;
    }
    auto inclusionFeeMapPtr = std::make_shared<InclusionFeeMap>();
    auto& inclusionFeeMap = *inclusionFeeMapPtr;
    int64_t baseFee = computeBaseFeeForLegacyTxSet(lclHeader, txList);
    for (auto const& tx : txList)
    {
        inclusionFeeMap[tx] = baseFee;
    }
    return TxSetPhaseFrame(std::move(txList), inclusionFeeMapPtr);
}

TxSetPhaseFrame
TxSetPhaseFrame::makeEmpty(bool isParallel)
{
    if (isParallel)
    {
        return TxSetPhaseFrame(TxStageFrameList{},
                               std::make_shared<InclusionFeeMap>());
    }
    return TxSetPhaseFrame(TxFrameList{}, std::make_shared<InclusionFeeMap>());
}

TxSetPhaseFrame::TxSetPhaseFrame(
    TxFrameList const& txs, std::shared_ptr<InclusionFeeMap> inclusionFeeMap)
    : mInclusionFeeMap(inclusionFeeMap), mIsParallel(false)
{
    if (!txs.empty())
    {
        mStages.emplace_back().push_back(txs);
    }
}

TxSetPhaseFrame::TxSetPhaseFrame(
    TxStageFrameList&& txs, std::shared_ptr<InclusionFeeMap> inclusionFeeMap)
    : mStages(txs), mInclusionFeeMap(inclusionFeeMap), mIsParallel(true)
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
TxSetPhaseFrame::size() const
{

    size_t size = 0;
    for (auto const& stage : mStages)
    {
        for (auto const& thread : stage)
        {
            size += thread.size();
        }
    }
    return size;
}

bool
TxSetPhaseFrame::empty() const
{
    return size() == 0;
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

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        parallelPhaseToXdr(mStages, *mInclusionFeeMap, xdrPhase);
#else
        releaseAssert(false);
#endif
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
        return TxSetPhaseFrame(sortedForApplyParallel(mStages, txSetHash),
                               mInclusionFeeMap);
    }
    else
    {
        return TxSetPhaseFrame(
            sortedForApplySequential(getSequentialTxs(), txSetHash),
            mInclusionFeeMap);
    }
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

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
ApplicableTxSetFrame::checkValid(Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    auto const& lcl = app.getLedgerManager().getLastClosedLedgerHeader();

    // Start by checking previousLedgerHash
    if (lcl.hash != mPreviousLedgerHash)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {}, expected {}",
                   hexAbbrev(mPreviousLedgerHash), hexAbbrev(lcl.hash));
        return false;
    }

    bool needGeneralizedTxSet = protocolVersionStartsFrom(
        lcl.header.ledgerVersion, SOROBAN_PROTOCOL_VERSION);
    if (needGeneralizedTxSet != isGeneralizedTxSet())
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet {}: need generalized '{}', expected '{}'",
                   hexAbbrev(mPreviousLedgerHash), needGeneralizedTxSet,
                   isGeneralizedTxSet());
        return false;
    }

    if (isGeneralizedTxSet())
    {
        auto checkFeeMap = [&](auto const& feeMap) {
            for (auto const& [tx, fee] : feeMap)
            {
                if (!fee)
                {
                    continue;
                }
                if (*fee < lcl.header.baseFee)
                {

                    CLOG_DEBUG(Herder,
                               "Got bad txSet: {} has too low component "
                               "base fee {}",
                               hexAbbrev(mPreviousLedgerHash), *fee);
                    return false;
                }
                if (tx->getInclusionFee() <
                    getMinInclusionFee(*tx, lcl.header, fee))
                {
                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: {} has tx with fee bid ({}) lower "
                        "than base fee ({})",
                        hexAbbrev(mPreviousLedgerHash), tx->getInclusionFee(),
                        getMinInclusionFee(*tx, lcl.header, fee));
                    return false;
                }
            }
            return true;
        };
        // Generalized transaction sets should always have 2 phases by
        // construction.
        releaseAssert(mPhases.size() ==
                      static_cast<size_t>(TxSetPhase::PHASE_COUNT));
        for (auto const& phase : mPhases)
        {
            if (!checkFeeMap(phase.getInclusionFeeMap()))
            {
                return false;
            }
        }
        if (mPhases[static_cast<size_t>(TxSetPhase::CLASSIC)].isParallel())
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: classic phase can't be parallel");
            return false;
        }
        bool needParallelSorobanPhase = protocolVersionStartsFrom(
            lcl.header.ledgerVersion, PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION);
        if (mPhases[static_cast<size_t>(TxSetPhase::SOROBAN)].isParallel() !=
            needParallelSorobanPhase)
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: Soroban phase parallel support "
                       "does not match the current protocol; '{}' was "
                       "expected",
                       needParallelSorobanPhase);
            return false;
        }
    }

    if (this->size(lcl.header, TxSetPhase::CLASSIC) > lcl.header.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many classic txs {} > {}",
                   this->size(lcl.header, TxSetPhase::CLASSIC),
                   lcl.header.maxTxSetSize);
        return false;
    }

    if (needGeneralizedTxSet)
    {
        // First, ensure the tx set does not contain multiple txs per source
        // account
        std::unordered_set<AccountID> seenAccounts;
        for (auto const& phaseTxs : mPhases)
        {
            for (auto const& tx : phaseTxs)
            {
                if (!seenAccounts.insert(tx->getSourceID()).second)
                {
                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: multiple txs per source account");
                    return false;
                }
            }
        }

        // Second, ensure total resources are not over ledger limit
        auto totalTxSetRes = getTxSetSorobanResource();
        if (!totalTxSetRes)
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: total Soroban resources overflow");
            return false;
        }

        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto limits = app.getLedgerManager().maxLedgerResources(
                /* isSoroban */ true);
            if (anyGreater(*totalTxSetRes, limits))
            {
                CLOG_DEBUG(Herder,
                           "Got bad txSet: needed resources exceed ledger "
                           "limits {} > {}",
                           totalTxSetRes->toString(), limits.toString());
                return false;
            }
        }
    }
    bool allValid = true;
    for (auto const& txs : mPhases)
    {
        if (!phaseTxsAreValid(txs, app, lowerBoundCloseTimeOffset,
                              upperBoundCloseTimeOffset))
        {
            allValid = false;
            break;
        }
    }
    return allValid;
}

size_t
ApplicableTxSetFrame::size(LedgerHeader const& lh,
                           std::optional<TxSetPhase> phase) const
{
    size_t sz = 0;
    if (!phase)
    {
        if (numPhases() > static_cast<size_t>(TxSetPhase::SOROBAN))
        {
            sz += sizeOp(TxSetPhase::SOROBAN);
        }
    }
    else if (phase.value() == TxSetPhase::SOROBAN)
    {
        sz += sizeOp(TxSetPhase::SOROBAN);
    }
    if (!phase || phase.value() == TxSetPhase::CLASSIC)
    {
        sz += protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11)
                  ? sizeOp(TxSetPhase::CLASSIC)
                  : sizeTx(TxSetPhase::CLASSIC);
    }
    return sz;
}

size_t
ApplicableTxSetFrame::sizeOp(TxSetPhase phase) const
{
    ZoneScoped;
    auto const& txs = mPhases.at(static_cast<size_t>(phase));
    return std::accumulate(txs.begin(), txs.end(), size_t(0),
                           [&](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

size_t
ApplicableTxSetFrame::sizeOpTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (size_t i = 0; i < mPhases.size(); i++)
    {
        total += sizeOp(static_cast<TxSetPhase>(i));
    }
    return total;
}

size_t
ApplicableTxSetFrame::sizeTx(TxSetPhase phase) const
{
    return mPhases.at(static_cast<size_t>(phase)).size();
}

size_t
ApplicableTxSetFrame::sizeTxTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (size_t i = 0; i < mPhases.size(); i++)
    {
        total += sizeTx(static_cast<TxSetPhase>(i));
    }
    return total;
}

std::optional<int64_t>
ApplicableTxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx) const
{
    for (auto const& phaseTxs : mPhases)
    {
        auto const& phaseMap = phaseTxs.getInclusionFeeMap();
        if (auto it = phaseMap.find(tx); it != phaseMap.end())
        {
            return it->second;
        }
    }
    throw std::runtime_error("Transaction not found in tx set");
}

std::optional<Resource>
ApplicableTxSetFrame::getTxSetSorobanResource() const
{
    releaseAssert(mPhases.size() > static_cast<size_t>(TxSetPhase::SOROBAN));
    auto total = Resource::makeEmptySoroban();
    for (auto const& tx : mPhases[static_cast<size_t>(TxSetPhase::SOROBAN)])
    {
        if (total.canAdd(tx->getResources(/* useByteLimitInClassic */ false)))
        {
            total += tx->getResources(/* useByteLimitInClassic */ false);
        }
        else
        {
            return std::nullopt;
        }
    }
    return std::make_optional<Resource>(total);
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
            total += tx->getFee(lh, getTxBaseFee(tx), true);
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
            total += tx->getInclusionFee();
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
        outputTxSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    }
    else
    {
        TransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        outputTxSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    }
    return outputTxSet;
}

bool
ApplicableTxSetFrame::isGeneralizedTxSet() const
{
    return mIsGeneralized;
}

} // namespace stellar
