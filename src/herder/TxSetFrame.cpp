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
bool
validateTxSetXDRStructure(GeneralizedTransactionSet const& txSet)
{
    if (txSet.v() != 1)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: unsupported version {}", txSet.v());
        return false;
    }
    auto const& txSetV1 = txSet.v1TxSet();
    // There was no protocol with 1 phase, so checking for 2 phases only
    if (txSetV1.phases.size() != static_cast<size_t>(TxSetPhase::PHASE_COUNT))
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet: exactly 2 phases are expected, got {}",
                   txSetV1.phases.size());
        return false;
    }

    for (auto const& phase : txSetV1.phases)
    {
        if (phase.v() != 0)
        {
            CLOG_DEBUG(Herder, "Got bad txSet: unsupported phase version {}",
                       phase.v());
            return false;
        }

        bool componentsNormalized = std::is_sorted(
            phase.v0Components().begin(), phase.v0Components().end(),
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
            std::adjacent_find(
                phase.v0Components().begin(), phase.v0Components().end(),
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
        // need to use the hash of whole tx here since multiple txs could have
        // the same Contents
        return lessThanXored(tx1->getFullHash(), tx2->getFullHash(), mSetHash);
    }
};

Hash
computeNonGenericTxSetContentsHash(TransactionSet const& xdrTxSet)
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

// Note: Soroban txs also use this functionality for simplicity, as it's a no-op
// (all Soroban txs have 1 op max)
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
transactionsToTransactionSetXDR(TxSetTransactions const& txs,
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
transactionsToGeneralizedTransactionSetXDR(
    TxSetPhaseTransactions const& phaseTxs,
    std::vector<std::unordered_map<TransactionFrameBaseConstPtr,
                                   std::optional<int64_t>>> const&
        phaseInclusionFeeMap,
    Hash const& previousLedgerHash, GeneralizedTransactionSet& generalizedTxSet)
{
    ZoneScoped;
    releaseAssert(phaseTxs.size() == phaseInclusionFeeMap.size());

    generalizedTxSet.v(1);
    generalizedTxSet.v1TxSet().previousLedgerHash = previousLedgerHash;

    for (int i = 0; i < phaseTxs.size(); ++i)
    {
        auto const& txPhase = phaseTxs[i];
        auto& phase =
            generalizedTxSet.v1TxSet().phases.emplace_back().v0Components();

        auto const& feeMap = phaseInclusionFeeMap[i];
        std::map<std::optional<int64_t>, size_t> feeTxCount;
        for (auto const& [tx, fee] : feeMap)
        {
            ++feeTxCount[fee];
        }
        // Reserve a component per unique base fee in order to have the correct
        // pointers in componentPerBid map.
        phase.reserve(feeTxCount.size());

        std::map<std::optional<int64_t>, xdr::xvector<TransactionEnvelope>*>
            componentPerBid;
        for (auto const& [fee, txCount] : feeTxCount)
        {
            phase.emplace_back(TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            auto& discountedFeeComponent = phase.back().txsMaybeDiscountedFee();
            if (fee)
            {
                discountedFeeComponent.baseFee.activate() = *fee;
            }
            componentPerBid[fee] = &discountedFeeComponent.txs;
            componentPerBid[fee]->reserve(txCount);
        }
        auto sortedTxs = TxSetUtils::sortTxsInHashOrder(txPhase);
        for (auto const& tx : sortedTxs)
        {
            componentPerBid[feeMap.find(tx)->second]->push_back(
                tx->getEnvelope());
        }
    }
}

// This assumes that the phase validation has already been done,
// specifically that there are no transactions that belong to the same
// source account, and that the ledger sequence corresponds to the
bool
phaseTxsAreValid(TxSetTransactions const& phase, Application& app,
                 uint64_t lowerBoundCloseTimeOffset,
                 uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    // This is done so minSeqLedgerGap is validated against the next
    // ledgerSeq, which is what will be used at apply time

    // Grab read-only latest ledger state; This is only used to validate tx sets
    // for LCL+1
    LedgerSnapshot ls(app);
    ls.getLedgerHeader().currentToModify().ledgerSeq =
        app.getLedgerManager().getLastClosedLedgerNum() + 1;
    for (auto const& tx : phase)
    {
        auto txResult = tx->checkValid(app.getAppConnector(), ls, 0,
                                       lowerBoundCloseTimeOffset,
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
} // namespace

TxSetXDRFrame::TxSetXDRFrame(TransactionSet const& xdrTxSet)
    : mXDRTxSet(xdrTxSet)
    , mEncodedSize(xdr::xdr_argpack_size(xdrTxSet))
    , mHash(computeNonGenericTxSetContentsHash(xdrTxSet))
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
makeTxSetFromTransactions(TxSetPhaseTransactions const& txPhases,
                          Application& app, uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset
#ifdef BUILD_TESTS
                          ,
                          bool skipValidation
#endif
)
{
    TxSetPhaseTransactions invalidTxs;
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
makeTxSetFromTransactions(TxSetPhaseTransactions const& txPhases,
                          Application& app, uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          TxSetPhaseTransactions& invalidTxs
#ifdef BUILD_TESTS
                          ,
                          bool skipValidation
#endif
)
{
    releaseAssert(txPhases.size() == invalidTxs.size());
    releaseAssert(txPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));

    TxSetPhaseTransactions validatedPhases;
    for (int i = 0; i < txPhases.size(); ++i)
    {
        auto& txs = txPhases[i];
        bool expectSoroban = static_cast<TxSetPhase>(i) == TxSetPhase::SOROBAN;
        if (!std::all_of(txs.begin(), txs.end(), [&](auto const& tx) {
                return tx->isSoroban() == expectSoroban;
            }))
        {
            throw std::runtime_error("TxSetFrame::makeFromTransactions: phases "
                                     "contain txs of wrong type");
        }

        auto& invalid = invalidTxs[i];
#ifdef BUILD_TESTS
        if (skipValidation)
        {
            validatedPhases.emplace_back(txs);
        }
        else
        {
#endif
            validatedPhases.emplace_back(
                TxSetUtils::trimInvalid(txs, app, lowerBoundCloseTimeOffset,
                                        upperBoundCloseTimeOffset, invalid));
#ifdef BUILD_TESTS
        }
#endif
    }

    auto const& lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    // Preliminary applicable frame - we don't know the contents hash yet, but
    // we also don't return this.
    std::unique_ptr<ApplicableTxSetFrame> preliminaryApplicableTxSet(
        new ApplicableTxSetFrame(app, lclHeader, validatedPhases,
                                 std::nullopt));
    preliminaryApplicableTxSet->applySurgePricing(app);
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
        for (int i = 0; i < preliminaryApplicableTxSet->numPhases(); ++i)
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
        TxSetPhaseTransactions emptyPhases(
            static_cast<size_t>(TxSetPhase::PHASE_COUNT));
        std::vector<std::unordered_map<TransactionFrameBaseConstPtr,
                                       std::optional<int64_t>>>
            emptyFeeMap(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
        GeneralizedTransactionSet txSet;
        transactionsToGeneralizedTransactionSetXDR(emptyPhases, emptyFeeMap,
                                                   lclHeader.hash, txSet);
        return TxSetXDRFrame::makeFromWire(txSet);
    }
    TransactionSet txSet;
    transactionsToTransactionSetXDR({}, lclHeader.hash, txSet);
    return TxSetXDRFrame::makeFromWire(txSet);
}

TxSetXDRFrameConstPtr
TxSetXDRFrame::makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                           TxSetTransactions const& txs)
{
    TransactionSet txSet;
    transactionsToTransactionSetXDR(txs, previousLedgerHash, txSet);
    return TxSetXDRFrame::makeFromWire(txSet);
}

#ifdef BUILD_TESTS
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxSetTransactions txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          bool enforceTxsApplyOrder)
{
    TxSetTransactions invalid;
    return makeTxSetFromTransactions(txs, app, lowerBoundCloseTimeOffset,
                                     upperBoundCloseTimeOffset, invalid,
                                     enforceTxsApplyOrder);
}

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxSetTransactions txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          TxSetTransactions& invalidTxs,
                          bool enforceTxsApplyOrder)
{
    auto lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    TxSetPhaseTransactions phases;
    phases.resize(protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                            SOROBAN_PROTOCOL_VERSION)
                      ? 2
                      : 1);
    for (auto& tx : txs)
    {
        if (tx->isSoroban())
        {
            phases[static_cast<size_t>(TxSetPhase::SOROBAN)].push_back(tx);
        }
        else
        {
            phases[static_cast<size_t>(TxSetPhase::CLASSIC)].push_back(tx);
        }
    }
    TxSetPhaseTransactions invalid;
    invalid.resize(phases.size());
    auto res = makeTxSetFromTransactions(phases, app, lowerBoundCloseTimeOffset,
                                         upperBoundCloseTimeOffset, invalid,
                                         enforceTxsApplyOrder);
    if (enforceTxsApplyOrder)
    {
        res.second->mApplyOrderOverride = txs;
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
    std::unique_ptr<ApplicableTxSetFrame> txSet{};
    if (isGeneralizedTxSet())
    {
        auto const& xdrTxSet = std::get<GeneralizedTransactionSet>(mXDRTxSet);
        if (!validateTxSetXDRStructure(xdrTxSet))
        {
            CLOG_DEBUG(Herder,
                       "Got bad generalized txSet with invalid XDR structure");
            return nullptr;
        }
        auto const& phases = xdrTxSet.v1TxSet().phases;
        TxSetPhaseTransactions defaultPhases;
        defaultPhases.resize(phases.size());

        txSet = std::unique_ptr<ApplicableTxSetFrame>(new ApplicableTxSetFrame(
            app, true, previousLedgerHash(), defaultPhases, mHash));

        releaseAssert(phases.size() <=
                      static_cast<size_t>(TxSetPhase::PHASE_COUNT));
        for (auto phaseId = 0; phaseId < phases.size(); phaseId++)
        {
            auto const& phase = phases[phaseId];
            auto const& components = phase.v0Components();
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
                    if (!txSet->addTxsFromXdr(
                            app.getNetworkID(),
                            component.txsMaybeDiscountedFee().txs, true,
                            baseFee, static_cast<TxSetPhase>(phaseId)))
                    {
                        CLOG_DEBUG(Herder,
                                   "Got bad generalized txSet: transactions "
                                   "are not ordered correctly or contain "
                                   "invalid phase transactions");
                        return nullptr;
                    }
                    break;
                }
            }
        }
    }
    else
    {
        auto const& xdrTxSet = std::get<TransactionSet>(mXDRTxSet);
        txSet = std::unique_ptr<ApplicableTxSetFrame>(new ApplicableTxSetFrame(
            app, false, previousLedgerHash(), {TxSetTransactions{}}, mHash));
        if (!txSet->addTxsFromXdr(app.getNetworkID(), xdrTxSet.txs, false,
                                  std::nullopt, TxSetPhase::CLASSIC))
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: transactions are not ordered correctly "
                       "or contain invalid phase transactions");
            return nullptr;
        }
        txSet->computeTxFeesForNonGeneralizedSet(
            app.getLedgerManager().getLastClosedLedgerHeader().header);
    }
    return txSet;
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
            for (auto const& component : phase.v0Components())
            {
                totalSize += component.txsMaybeDiscountedFee().txs.size();
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
            for (auto const& component : phase.v0Components())
            {
                totalSize += std::accumulate(
                    component.txsMaybeDiscountedFee().txs.begin(),
                    component.txsMaybeDiscountedFee().txs.end(), 0ull,
                    accumulateTxsFn);
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

TxSetPhaseTransactions
TxSetXDRFrame::createTransactionFrames(Hash const& networkID) const
{
    TxSetPhaseTransactions phaseTxs;
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        for (auto const& phase : txSet.phases)
        {
            auto& txs = phaseTxs.emplace_back();
            for (auto const& component : phase.v0Components())
            {
                for (auto const& tx : component.txsMaybeDiscountedFee().txs)
                {
                    txs.emplace_back(
                        TransactionFrameBase::makeTransactionFromWire(networkID,
                                                                      tx));
                }
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

ApplicableTxSetFrame::ApplicableTxSetFrame(Application& app, bool isGeneralized,
                                           Hash const& previousLedgerHash,
                                           TxSetPhaseTransactions const& txs,
                                           std::optional<Hash> contentsHash)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTxPhases(txs)
    , mPhaseInclusionFeeMap(mTxPhases.size())
    , mContentsHash(contentsHash)
{
    releaseAssert(previousLedgerHash ==
                  app.getLedgerManager().getLastClosedLedgerHeader().hash);
}

ApplicableTxSetFrame::ApplicableTxSetFrame(
    Application& app, LedgerHeaderHistoryEntry const& lclHeader,
    TxSetPhaseTransactions const& txs, std::optional<Hash> contentsHash)
    : ApplicableTxSetFrame(
          app,
          protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                    SOROBAN_PROTOCOL_VERSION),
          lclHeader.hash, txs, contentsHash)
{
}

Hash const&
ApplicableTxSetFrame::getContentsHash() const
{
    releaseAssert(mContentsHash);
    return *mContentsHash;
}

TxSetTransactions const&
ApplicableTxSetFrame::getTxsForPhase(TxSetPhase phase) const
{
    releaseAssert(static_cast<size_t>(phase) < mTxPhases.size());
    return mTxPhases.at(static_cast<size_t>(phase));
}

TxSetTransactions
ApplicableTxSetFrame::getTxsInApplyOrder() const
{
#ifdef BUILD_TESTS
    if (mApplyOrderOverride)
    {
        return *mApplyOrderOverride;
    }
#endif
    ZoneScoped;

    // Use a single vector to order transactions from all phases
    std::vector<TransactionFrameBasePtr> retList;
    retList.reserve(sizeTxTotal());

    for (auto const& phase : mTxPhases)
    {
        auto txQueues = TxSetUtils::buildAccountTxQueues(phase);

        // build txBatches
        // txBatches i-th element contains each i-th transaction for accounts
        // with a transaction in the transaction set
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
            ApplyTxSorter s(getContentsHash());
            std::sort(batch.begin(), batch.end(), s);
            for (auto const& tx : batch)
            {
                retList.push_back(tx);
            }
        }
    }

    return retList;
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
    auto& lcl = app.getLedgerManager().getLastClosedLedgerHeader();

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

                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: {} has too low component base fee {}",
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

        if (!checkFeeMap(getInclusionFeeMap(TxSetPhase::CLASSIC)))
        {
            return false;
        }
        if (!checkFeeMap(getInclusionFeeMap(TxSetPhase::SOROBAN)))
        {
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
        for (auto const& phase : mTxPhases)
        {
            for (auto const& tx : phase)
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
    for (auto const& txs : mTxPhases)
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
    auto const& txs = mTxPhases.at(static_cast<size_t>(phase));
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
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        total += sizeOp(static_cast<TxSetPhase>(i));
    }
    return total;
}

size_t
ApplicableTxSetFrame::sizeTxTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        total += sizeTx(static_cast<TxSetPhase>(i));
    }
    return total;
}

void
ApplicableTxSetFrame::computeTxFeesForNonGeneralizedSet(
    LedgerHeader const& lclHeader)
{
    ZoneScoped;
    auto ledgerVersion = lclHeader.ledgerVersion;
    int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
    releaseAssert(mTxPhases.size() == 1);
    for (auto const& txPtr : mTxPhases[0])
    {
        int64_t txBaseFee = computePerOpFee(*txPtr, ledgerVersion);
        lowBaseFee = std::min(lowBaseFee, txBaseFee);
    }
    computeTxFeesForNonGeneralizedSet(lclHeader, lowBaseFee,
                                      /* enableLogging */ false);
}

void
ApplicableTxSetFrame::computeTxFeesForNonGeneralizedSet(
    LedgerHeader const& lclHeader, int64_t lowestBaseFee, bool enableLogging)
{
    ZoneScoped;
    int64_t baseFee = lclHeader.baseFee;

    if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                  ProtocolVersion::V_11))
    {
        size_t surgeOpsCutoff = 0;
        if (lclHeader.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lclHeader.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (sizeOp(TxSetPhase::CLASSIC) > surgeOpsCutoff)
        {
            baseFee = lowestBaseFee;
            if (enableLogging)
            {
                CLOG_WARNING(Herder, "surge pricing in effect! {} > {}",
                             sizeOp(TxSetPhase::CLASSIC), surgeOpsCutoff);
            }
        }
    }

    releaseAssert(mTxPhases.size() == 1);
    releaseAssert(mPhaseInclusionFeeMap.size() == 1);
    auto const& phase = mTxPhases[static_cast<size_t>(TxSetPhase::CLASSIC)];
    auto& feeMap = getInclusionFeeMapMut(TxSetPhase::CLASSIC);
    for (auto const& tx : phase)
    {
        feeMap[tx] = baseFee;
    }
}

void
ApplicableTxSetFrame::computeTxFees(
    TxSetPhase phase, LedgerHeader const& ledgerHeader,
    SurgePricingLaneConfig const& surgePricingConfig,
    std::vector<int64_t> const& lowestLaneFee,
    std::vector<bool> const& hadTxNotFittingLane)
{
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(lowestLaneFee.size() == hadTxNotFittingLane.size());
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

    auto const& txs = mTxPhases.at(static_cast<size_t>(phase));
    auto& feeMap = getInclusionFeeMapMut(phase);
    for (auto const& tx : txs)
    {
        feeMap[tx] = laneBaseFee[surgePricingConfig.getLane(*tx)];
    }
}

std::optional<int64_t>
ApplicableTxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                   LedgerHeader const& lclHeader) const
{
    for (auto const& phaseMap : mPhaseInclusionFeeMap)
    {
        if (auto it = phaseMap.find(tx); it != phaseMap.end())
        {
            return it->second;
        }
    }
    throw std::runtime_error("Transaction not found in tx set");
    return std::nullopt;
}

std::optional<Resource>
ApplicableTxSetFrame::getTxSetSorobanResource() const
{
    releaseAssert(mTxPhases.size() > static_cast<size_t>(TxSetPhase::SOROBAN));
    auto total = Resource::makeEmptySoroban();
    for (auto const& tx : mTxPhases[static_cast<size_t>(TxSetPhase::SOROBAN)])
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
    std::for_each(mTxPhases.begin(), mTxPhases.end(),
                  [&](TxSetTransactions const& phase) {
                      total += std::accumulate(
                          phase.begin(), phase.end(), int64_t(0),
                          [&](int64_t t, TransactionFrameBasePtr const& tx) {
                              return t +
                                     tx->getFee(lh, getTxBaseFee(tx, lh), true);
                          });
                  });

    return total;
}

int64_t
ApplicableTxSetFrame::getTotalInclusionFees() const
{
    ZoneScoped;
    int64_t total{0};
    std::for_each(mTxPhases.begin(), mTxPhases.end(),
                  [&](TxSetTransactions const& phase) {
                      total += std::accumulate(
                          phase.begin(), phase.end(), int64_t(0),
                          [&](int64_t t, TransactionFrameBasePtr const& tx) {
                              return t + tx->getInclusionFee();
                          });
                  });

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
            getInclusionFeeMap(TxSetPhase::CLASSIC)
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
    releaseAssert(mTxPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));
    for (auto i = 0; i != mTxPhases.size(); i++)
    {
        if (!status.empty())
        {
            status += ", ";
        }
        status += fmt::format(
            FMT_STRING("{} phase: {}"),
            getTxSetPhaseName(static_cast<TxSetPhase>(i)),
            feeStats(getInclusionFeeMap(static_cast<TxSetPhase>(i))));
    }
    return status;
}

void
ApplicableTxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    releaseAssert(mTxPhases.size() == 1);
    transactionsToTransactionSetXDR(mTxPhases[0], mPreviousLedgerHash, txSet);
}

void
ApplicableTxSetFrame::toXDR(GeneralizedTransactionSet& generalizedTxSet) const
{
    ZoneScoped;
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(mTxPhases.size() <=
                  static_cast<size_t>(TxSetPhase::PHASE_COUNT));
    transactionsToGeneralizedTransactionSetXDR(mTxPhases, mPhaseInclusionFeeMap,
                                               mPreviousLedgerHash,
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

bool
ApplicableTxSetFrame::addTxsFromXdr(
    Hash const& networkID, xdr::xvector<TransactionEnvelope> const& txs,
    bool useBaseFee, std::optional<int64_t> baseFee, TxSetPhase phase)
{
    auto& phaseTxs = mTxPhases.at(static_cast<int>(phase));
    size_t oldSize = phaseTxs.size();
    phaseTxs.reserve(oldSize + txs.size());
    auto& inclusionFeeMap = getInclusionFeeMapMut(phase);
    for (auto const& env : txs)
    {
        auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, env);
        if (!tx->XDRProvidesValidFee())
        {
            return false;
        }
        // Phase should be consistent with the tx we're trying to add
        if ((tx->isSoroban() && phase != TxSetPhase::SOROBAN) ||
            (!tx->isSoroban() && phase != TxSetPhase::CLASSIC))
        {
            return false;
        }

        phaseTxs.push_back(tx);
        if (useBaseFee)
        {
            inclusionFeeMap[tx] = baseFee;
        }
    }
    return std::is_sorted(phaseTxs.begin() + oldSize, phaseTxs.end(),
                          &TxSetUtils::hashTxSorter);
}

void
ApplicableTxSetFrame::applySurgePricing(Application& app)
{
    ZoneScoped;
    releaseAssert(mTxPhases.size() <=
                  static_cast<int>(TxSetPhase::PHASE_COUNT));
    auto const& lclHeader =
        app.getLedgerManager().getLastClosedLedgerHeader().header;
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        TxSetPhase phaseType = static_cast<TxSetPhase>(i);
        auto& phase = mTxPhases[i];
        if (phaseType == TxSetPhase::CLASSIC)
        {
            auto maxOps =
                Resource({static_cast<uint32_t>(
                              app.getLedgerManager().getLastMaxTxSetSizeOps()),
                          MAX_CLASSIC_BYTE_ALLOWANCE});
            std::optional<Resource> dexOpsLimit;
            if (isGeneralizedTxSet() &&
                app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET)
            {
                // DEX operations limit implies that DEX transactions should
                // compete with each other in in a separate fee lane, which is
                // only possible with generalized tx set.
                dexOpsLimit =
                    Resource({*app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET,
                              MAX_CLASSIC_BYTE_ALLOWANCE});
            }

            auto surgePricingLaneConfig =
                std::make_shared<DexLimitingLaneConfig>(maxOps, dexOpsLimit);

            std::vector<bool> hadTxNotFittingLane;

            auto includedTxs =
                SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
                    phase, surgePricingLaneConfig, hadTxNotFittingLane);

            size_t laneCount = surgePricingLaneConfig->getLaneLimits().size();
            std::vector<int64_t> lowestLaneFee(
                laneCount, std::numeric_limits<int64_t>::max());
            for (auto const& tx : includedTxs)
            {
                size_t lane = surgePricingLaneConfig->getLane(*tx);
                auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
                lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
            }

            phase = includedTxs;
            if (isGeneralizedTxSet())
            {
                computeTxFees(TxSetPhase::CLASSIC, lclHeader,
                              *surgePricingLaneConfig, lowestLaneFee,
                              hadTxNotFittingLane);
            }
            else
            {
                computeTxFeesForNonGeneralizedSet(
                    lclHeader,
                    lowestLaneFee[SurgePricingPriorityQueue::GENERIC_LANE],
                    /* enableLogging */ true);
            }
        }
        else
        {
            releaseAssert(isGeneralizedTxSet());
            releaseAssert(phaseType == TxSetPhase::SOROBAN);

            auto limits = app.getLedgerManager().maxLedgerResources(
                /* isSoroban */ true);

            auto byteLimit =
                std::min(static_cast<int64_t>(MAX_SOROBAN_BYTE_ALLOWANCE),
                         limits.getVal(Resource::Type::TX_BYTE_SIZE));
            limits.setVal(Resource::Type::TX_BYTE_SIZE, byteLimit);

            auto surgePricingLaneConfig =
                std::make_shared<SorobanGenericLaneConfig>(limits);

            std::vector<bool> hadTxNotFittingLane;
            auto includedTxs =
                SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
                    phase, surgePricingLaneConfig, hadTxNotFittingLane);

            size_t laneCount = surgePricingLaneConfig->getLaneLimits().size();
            std::vector<int64_t> lowestLaneFee(
                laneCount, std::numeric_limits<int64_t>::max());
            for (auto const& tx : includedTxs)
            {
                size_t lane = surgePricingLaneConfig->getLane(*tx);
                auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
                lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
            }

            phase = includedTxs;
            computeTxFees(phaseType, lclHeader, *surgePricingLaneConfig,
                          lowestLaneFee, hadTxNotFittingLane);
        }
    }
}

std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>> const&
ApplicableTxSetFrame::getInclusionFeeMap(TxSetPhase phase) const
{
    size_t phaseId = static_cast<size_t>(phase);
    releaseAssert(phaseId < mPhaseInclusionFeeMap.size());
    return mPhaseInclusionFeeMap[phaseId];
}

std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>&
ApplicableTxSetFrame::getInclusionFeeMapMut(TxSetPhase phase)
{
    size_t phaseId = static_cast<size_t>(phase);
    releaseAssert(phaseId < mPhaseInclusionFeeMap.size());
    return mPhaseInclusionFeeMap[phaseId];
}

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
} // namespace stellar
