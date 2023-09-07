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
// The frame created around malformed transaction set XDR received over the
// wire.
// This does not initialize the internal data structures, but does store the XDR
// message itself. This is needed to support a specific use-case: transaction
// sets may be requested by the peers even when they are malformed and we need
// to provide the message they requested for.
class InvalidTxSetFrame : public TxSetFrame
{
  public:
    template <typename T>
    InvalidTxSetFrame(T const& xdrTxSet, Hash const& hash, size_t encodedSize)
        : TxSetFrame(std::is_same_v<T, GeneralizedTransactionSet>, {},
                     std::is_same_v<T, GeneralizedTransactionSet>
                         ? TxSetFrame::TxPhases{TxSetFrame::Transactions{},
                                                TxSetFrame::Transactions{}}
                         : TxSetFrame::TxPhases{TxSetFrame::Transactions{}})
        , mXDRTxSet(xdrTxSet)
    {
        mHash = hash;
        mEncodedSize = encodedSize;
    }

    bool
    checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const override
    {
        return false;
    }

    void
    toXDR(TransactionSet& txSet) const override
    {
        releaseAssert(std::holds_alternative<TransactionSet>(mXDRTxSet));
        txSet = std::get<TransactionSet>(mXDRTxSet);
    }

    void
    toXDR(GeneralizedTransactionSet& generalizedTxSet) const override
    {
        releaseAssert(
            std::holds_alternative<GeneralizedTransactionSet>(mXDRTxSet));
        generalizedTxSet = std::get<GeneralizedTransactionSet>(mXDRTxSet);
    }

#ifdef BUILD_TESTS
    bool
    checkValidStructure() const override
    {
        return false;
    }
#endif

  private:
    std::variant<TransactionSet, GeneralizedTransactionSet> mXDRTxSet;
};

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
    if (txSetV1.phases.size() !=
        static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT))
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
    auto rounding = protocolVersionStartsFrom(
                        ledgerVersion, GENERALIZED_TX_SET_PROTOCOL_VERSION)
                        ? Rounding::ROUND_DOWN
                        : Rounding::ROUND_UP;
    auto txOps = tx.getNumOperations();
    return bigDivideOrThrow(tx.getInclusionFee(), 1,
                            static_cast<int64_t>(txOps), rounding);
}

} // namespace

TxSetFrame::TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
                       TxPhases const& txs)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTxPhases(txs)
{
    mFeesComputed.resize(mTxPhases.size(), false);
}

#ifdef BUILD_TESTS
bool
TxSetFrame::checkValidStructure() const
{
    return true;
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxSetFrame::Transactions txs, Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset)
{
    Transactions invalid;
    return TxSetFrame::makeFromTransactions(txs, app, lowerBoundCloseTimeOffset,
                                            upperBoundCloseTimeOffset, invalid);
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(Transactions txs, Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 Transactions& invalidTxs)
{
    TxSetFrame::TxPhases phases;
    phases.emplace_back(txs);
    auto lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    if (protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                  GENERALIZED_TX_SET_PROTOCOL_VERSION))
    {
        // Empty soroban phase
        phases.emplace_back();
    }
    TxSetFrame::TxPhases invalid;
    invalid.resize(phases.size());
    auto res =
        TxSetFrame::makeFromTransactions(phases, app, lowerBoundCloseTimeOffset,
                                         upperBoundCloseTimeOffset, invalid);
    invalidTxs = invalid[0];
    return res;
}
#endif

TxSetFrame::TxSetFrame(LedgerHeaderHistoryEntry const& lclHeader,
                       TxPhases const& txs)
    : TxSetFrame(protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                           GENERALIZED_TX_SET_PROTOCOL_VERSION),
                 lclHeader.hash, txs)
{
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxPhases const& txPhases, Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset)
{
    TxPhases invalidTxs;
    invalidTxs.resize(txPhases.size());
    return makeFromTransactions(txPhases, app, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset, invalidTxs);
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxPhases const& txPhases, Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 TxPhases& invalidTxs)
{
    releaseAssert(txPhases.size() == invalidTxs.size());
    releaseAssert(txPhases.size() <=
                  static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT));

    TxPhases validatedPhases;
    for (int i = 0; i < txPhases.size(); ++i)
    {
        auto& txs = txPhases[i];
        bool expectSoroban = static_cast<Phase>(i) == Phase::SOROBAN;
        if (!std::all_of(txs.begin(), txs.end(), [&](auto const& tx) {
                return tx->isSoroban() == expectSoroban;
            }))
        {
            throw std::runtime_error("TxSetFrame::makeFromTransactions: phases "
                                     "contain txs of wrong type");
        }

        auto& invalid = invalidTxs[i];
        validatedPhases.emplace_back(
            TxSetUtils::trimInvalid(txs, app, lowerBoundCloseTimeOffset,
                                    upperBoundCloseTimeOffset, invalid));
    }

    auto const& lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    // We can't use `std::make_shared` here as the constructors are protected.
    // This may cause leaks in case of exceptions, so keep the constructors
    // simple and exception-safe.
    std::shared_ptr<TxSetFrame> txSet(
        new TxSetFrame(lclHeader, validatedPhases));
    txSet->applySurgePricing(app);

    // Do the roundtrip through XDR to ensure we never build an incorrect tx set
    // for nomination.
    TxSetFrameConstPtr outputTxSet;
    if (txSet->isGeneralizedTxSet())
    {
        GeneralizedTransactionSet xdrTxSet;
        txSet->toXDR(xdrTxSet);
        outputTxSet = TxSetFrame::makeFromWire(app, xdrTxSet);
    }
    else
    {
        TransactionSet xdrTxSet;
        txSet->toXDR(xdrTxSet);
        outputTxSet = TxSetFrame::makeFromWire(app, xdrTxSet);
    }
    // Make sure no transactions were lost during the roundtrip and the output
    // tx set is valid.
    bool valid = txSet->numPhases() == outputTxSet->numPhases();
    if (valid)
    {
        for (int i = 0; i < txSet->numPhases(); ++i)
        {
            valid = valid && txSet->sizeTx(static_cast<Phase>(i)) ==
                                 outputTxSet->sizeTx(static_cast<Phase>(i));
        }
    }
    valid = valid && outputTxSet->checkValid(app, lowerBoundCloseTimeOffset,
                                             upperBoundCloseTimeOffset);
    if (!valid)
    {
        throw std::runtime_error("Created invalid tx set frame");
    }
    return outputTxSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                        Transactions const& txs)
{
    // We can't use `std::make_shared` here as the constructors are protected.
    // This may cause leaks in case of exceptions, so keep the constructors
    // simple and exception-safe.
    return std::shared_ptr<TxSetFrame>(
        new TxSetFrame(false, previousLedgerHash, TxSetFrame::TxPhases{txs}));
}

TxSetFrameConstPtr
TxSetFrame::makeEmpty(LedgerHeaderHistoryEntry const& lclHeader)
{
    // We can't use `std::make_shared` here as the constructors are protected.
    // This may cause leaks in case of exceptions, so keep the constructors
    // simple and exception-safe.
    TxPhases phases;
    phases.resize(protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                            GENERALIZED_TX_SET_PROTOCOL_VERSION)
                      ? static_cast<size_t>(Phase::PHASE_COUNT)
                      : 1);
    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(lclHeader, phases));
    for (int i = 0; i < txSet->mFeesComputed.size(); i++)
    {
        txSet->mFeesComputed[i] = true;
    }
    txSet->computeContentsHash();
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(Application& app, TransactionSet const& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(
        false, xdrTxSet.previousLedgerHash, {TxSetFrame::Transactions{}}));
    size_t encodedSize = xdr::xdr_argpack_size(xdrTxSet);
    if (!txSet->addTxsFromXdr(app, xdrTxSet.txs, false, std::nullopt,
                              Phase::CLASSIC))
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet: transactions are not "
                   "ordered correctly or contain invalid phase transactions");
        return std::make_shared<InvalidTxSetFrame const>(
            xdrTxSet, computeNonGenericTxSetContentsHash(xdrTxSet),
            encodedSize);
    }
    txSet->computeContentsHash();
    txSet->mEncodedSize = encodedSize;
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(Application& app,
                         GeneralizedTransactionSet const& xdrTxSet)
{
    ZoneScoped;
    auto hash = xdrSha256(xdrTxSet);
    size_t encodedSize = xdr::xdr_argpack_size(xdrTxSet);
    if (!validateTxSetXDRStructure(xdrTxSet))
    {
        return std::make_shared<InvalidTxSetFrame const>(xdrTxSet, hash,
                                                         encodedSize);
    }

    auto const& phases = xdrTxSet.v1TxSet().phases;
    TxPhases defaultPhases;
    defaultPhases.resize(phases.size(), TxSetFrame::Transactions{});

    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(
        true, xdrTxSet.v1TxSet().previousLedgerHash, defaultPhases));
    // Mark fees as already computed as we read them from the XDR.
    for (int i = 0; i < txSet->mFeesComputed.size(); i++)
    {
        txSet->mFeesComputed[i] = true;
    }
    txSet->mHash = hash;
    releaseAssert(phases.size() <= static_cast<size_t>(Phase::PHASE_COUNT));
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
                        app, component.txsMaybeDiscountedFee().txs, true,
                        baseFee, static_cast<Phase>(phaseId)))
                {
                    CLOG_DEBUG(Herder, "Got bad txSet: transactions are not "
                                       "ordered correctly or contain invalid "
                                       "phase transactions");
                    return std::make_shared<InvalidTxSetFrame const>(
                        xdrTxSet, hash, encodedSize);
                }
                break;
            }
        }
    }
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromStoredTxSet(StoredTransactionSet const& storedSet,
                                Application& app)
{
    TxSetFrameConstPtr cur;
    if (storedSet.v() == 0)
    {
        cur = TxSetFrame::makeFromWire(app, storedSet.txSet());
    }
    else
    {
        cur = TxSetFrame::makeFromWire(app, storedSet.generalizedTxSet());
    }

    return cur;
}

Hash const&
TxSetFrame::getContentsHash() const
{
    releaseAssert(mHash);
    return *mHash;
}

Hash const&
TxSetFrame::previousLedgerHash() const
{
    return mPreviousLedgerHash;
}

TxSetFrame::Transactions const&
TxSetFrame::getTxsForPhase(Phase phase) const
{
    releaseAssert(static_cast<size_t>(phase) < mTxPhases.size());
    return mTxPhases.at(static_cast<size_t>(phase));
}

TxSetFrame::Transactions
TxSetFrame::getTxsInApplyOrder() const
{
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
TxSetFrame::checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
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
        lcl.header.ledgerVersion, GENERALIZED_TX_SET_PROTOCOL_VERSION);
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
        releaseAssert(std::all_of(mFeesComputed.begin(), mFeesComputed.end(),
                                  [](bool comp) { return comp; }));
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

        if (!checkFeeMap(getInclusionFeeMap(Phase::CLASSIC)))
        {
            return false;
        }
        if (!checkFeeMap(getInclusionFeeMap(Phase::SOROBAN)))
        {
            return false;
        }
    }

    if (this->size(lcl.header, Phase::CLASSIC) > lcl.header.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many classic txs {} > {}",
                   this->size(lcl.header, Phase::CLASSIC),
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
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto limits = app.getLedgerManager().maxLedgerResources(
                /* isSoroban */ true, ltx);
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
        if (!TxSetUtils::getInvalidTxList(txs, app, lowerBoundCloseTimeOffset,
                                          upperBoundCloseTimeOffset, true)
                 .empty())
        {
            allValid = false;
            break;
        }
    }
    return allValid;
}

size_t
TxSetFrame::size(LedgerHeader const& lh, std::optional<Phase> phase) const
{
    size_t sz = 0;
    if (protocolVersionStartsFrom(lh.ledgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        if (!phase)
        {
            if (numPhases() > static_cast<size_t>(Phase::SOROBAN))
            {
                sz += sizeOp(Phase::SOROBAN);
            }
        }
        else if (phase.value() == Phase::SOROBAN)
        {
            sz += sizeOp(Phase::SOROBAN);
        }
    }
    if (!phase || phase.value() == Phase::CLASSIC)
    {
        sz += protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11)
                  ? sizeOp(Phase::CLASSIC)
                  : sizeTx(Phase::CLASSIC);
    }
    return sz;
}

size_t
TxSetFrame::sizeOp(Phase phase) const
{
    ZoneScoped;
    auto const& txs = mTxPhases.at(static_cast<size_t>(phase));
    return std::accumulate(txs.begin(), txs.end(), size_t(0),
                           [&](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

size_t
TxSetFrame::sizeOpTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        total += sizeOp(static_cast<Phase>(i));
    }
    return total;
}

size_t
TxSetFrame::sizeTxTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        total += sizeTx(static_cast<Phase>(i));
    }
    return total;
}

size_t
TxSetFrame::encodedSize() const
{
    if (mEncodedSize)
    {
        return *mEncodedSize;
    }
    ZoneScoped;
    if (isGeneralizedTxSet())
    {
        GeneralizedTransactionSet encoded;
        toXDR(encoded);
        mEncodedSize = xdr::xdr_argpack_size(encoded);
    }
    else
    {
        TransactionSet encoded;
        toXDR(encoded);
        mEncodedSize = xdr::xdr_argpack_size(encoded);
    }
    return *mEncodedSize;
}

void
TxSetFrame::computeTxFeesForNonGeneralizedSet(
    LedgerHeader const& lclHeader) const
{
    ZoneScoped;
    auto ledgerVersion = lclHeader.ledgerVersion;
    int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
    releaseAssert(mTxPhases.size() == 1);
    for (auto& txPtr : mTxPhases[0])
    {
        int64_t txBaseFee = computePerOpFee(*txPtr, ledgerVersion);
        lowBaseFee = std::min(lowBaseFee, txBaseFee);
    }
    computeTxFeesForNonGeneralizedSet(lclHeader, lowBaseFee,
                                      /* enableLogging */ false);
}

void
TxSetFrame::computeTxFeesForNonGeneralizedSet(LedgerHeader const& lclHeader,
                                              int64_t lowestBaseFee,
                                              bool enableLogging) const
{
    ZoneScoped;
    releaseAssert(std::none_of(mFeesComputed.begin(), mFeesComputed.end(),
                               [](bool comp) { return comp; }));
    int64_t baseFee = lclHeader.baseFee;

    if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                  ProtocolVersion::V_11))
    {
        size_t surgeOpsCutoff = 0;
        if (lclHeader.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lclHeader.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (sizeOp(Phase::CLASSIC) > surgeOpsCutoff)
        {
            baseFee = lowestBaseFee;
            if (enableLogging)
            {
                CLOG_WARNING(Herder, "surge pricing in effect! {} > {}",
                             sizeOp(Phase::CLASSIC), surgeOpsCutoff);
            }
        }
    }

    releaseAssert(mTxPhases.size() == 1);
    auto const& phase = mTxPhases[0];

    for (auto const& tx : phase)
    {
        mTxBaseFeeClassic[tx] = baseFee;
    }
    mFeesComputed[0] = true;
}

void
TxSetFrame::computeTxFees(TxSetFrame::Phase phase,
                          LedgerHeader const& ledgerHeader,
                          SurgePricingLaneConfig const& surgePricingConfig,
                          std::vector<int64_t> const& lowestLaneFee,
                          std::vector<bool> const& hadTxNotFittingLane)
{
    releaseAssert(!mFeesComputed[phase]);
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
                getPhaseName(phase),
                lane == SurgePricingPriorityQueue::GENERIC_LANE ? "generic"
                                                                : "DEX",
                laneBaseFee[lane], ledgerHeader.baseFee);
        }
    }

    auto const& txs = mTxPhases.at(phase);
    auto& feeMap = getInclusionFeeMap(phase);
    for (auto const& tx : txs)
    {
        feeMap[tx] = laneBaseFee[surgePricingConfig.getLane(*tx)];
    }
    mFeesComputed[phase] = true;
}

std::optional<int64_t>
TxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                         LedgerHeader const& lclHeader) const
{
    if (std::any_of(mFeesComputed.begin(), mFeesComputed.end(),
                    [](bool comp) { return !comp; }))
    {
        releaseAssert(!isGeneralizedTxSet());
        computeTxFeesForNonGeneralizedSet(lclHeader);
    }
    auto it = mTxBaseFeeClassic.find(tx);
    if (it == mTxBaseFeeClassic.end())
    {
        it = mTxBaseFeeSoroban.find(tx);
        if (it == mTxBaseFeeSoroban.end())
        {
            throw std::runtime_error("Transaction not found in tx set");
        }
    }
    return it->second;
}

std::optional<Resource>
TxSetFrame::getTxSetSorobanResource() const
{
    releaseAssert(mTxPhases.size() > static_cast<size_t>(Phase::SOROBAN));
    auto total = Resource::makeEmpty(/* isSoroban */ true);
    for (auto const& tx : mTxPhases[static_cast<size_t>(Phase::SOROBAN)])
    {
        if (total.canAdd(tx->getResources()))
        {
            total += tx->getResources();
        }
        else
        {
            return std::nullopt;
        }
    }
    return std::make_optional<Resource>(total);
}

int64_t
TxSetFrame::getTotalFees(LedgerHeader const& lh) const
{
    ZoneScoped;
    int64_t total{0};
    std::for_each(
        mTxPhases.begin(), mTxPhases.end(), [&](Transactions const& phase) {
            total += std::accumulate(
                phase.begin(), phase.end(), int64_t(0),
                [&](int64_t t, TransactionFrameBasePtr const& tx) {
                    return t + tx->getFee(lh, getTxBaseFee(tx, lh), true);
                });
        });

    return total;
}

int64_t
TxSetFrame::getTotalInclusionFees() const
{
    ZoneScoped;
    int64_t total{0};
    std::for_each(mTxPhases.begin(), mTxPhases.end(),
                  [&](Transactions const& phase) {
                      total += std::accumulate(
                          phase.begin(), phase.end(), int64_t(0),
                          [&](int64_t t, TransactionFrameBasePtr const& tx) {
                              return t + tx->getInclusionFee();
                          });
                  });

    return total;
}

std::string
TxSetFrame::summary() const
{
    if (empty())
    {
        return "empty tx set";
    }
    if (isGeneralizedTxSet())
    {
        auto feeStats = [&](auto const& feeMap) {
            std::map<std::optional<int64_t>, std::pair<int, int>>
                componentStats;
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
                        FMT_STRING(
                            "{{discounted txs:{}, ops:{}, base_fee:{}}}"),
                        stats.first, stats.second, *fee);
                }
                else
                {
                    res += fmt::format(
                        FMT_STRING("{{non-discounted txs:{}, ops:{}}}"),
                        stats.first, stats.second);
                }
            }
            res += "]";
            return res;
        };

        std::string status;
        releaseAssert(mTxPhases.size() <=
                      static_cast<size_t>(Phase::PHASE_COUNT));
        for (auto i = 0; i != mTxPhases.size(); i++)
        {
            if (!status.empty())
            {
                status += ", ";
            }
            status += fmt::format(
                FMT_STRING("{} phase: {}"), getPhaseName(static_cast<Phase>(i)),
                feeStats(getInclusionFeeMap(static_cast<Phase>(i))));
        }
        return status;
    }
    else
    {
        return fmt::format(FMT_STRING("txs:{}, ops:{}, base_fee:{}"),
                           sizeTxTotal(), sizeOpTotal(),
                           *mTxBaseFeeClassic.begin()->second);
    }
}

void
TxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    releaseAssert(mTxPhases.size() == 1);
    auto& txs = mTxPhases[0];
    txSet.txs.resize(xdr::size32(txs.size()));
    auto sortedTxs = TxSetUtils::sortTxsInHashOrder(txs);
    for (unsigned int n = 0; n < sortedTxs.size(); n++)
    {
        txSet.txs[n] = sortedTxs[n]->getEnvelope();
    }
    txSet.previousLedgerHash = mPreviousLedgerHash;
}

void
TxSetFrame::toXDR(GeneralizedTransactionSet& generalizedTxSet) const
{
    ZoneScoped;
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(std::all_of(mFeesComputed.begin(), mFeesComputed.end(),
                              [](bool comp) { return comp; }));
    releaseAssert(mTxPhases.size() <= static_cast<size_t>(Phase::PHASE_COUNT));

    generalizedTxSet.v(1);
    generalizedTxSet.v1TxSet().previousLedgerHash = mPreviousLedgerHash;

    for (int i = 0; i < mTxPhases.size(); ++i)
    {
        auto const& txPhase = mTxPhases[i];
        auto& phase =
            generalizedTxSet.v1TxSet().phases.emplace_back().v0Components();

        auto const& feeMap = getInclusionFeeMap(static_cast<Phase>(i));
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

bool
TxSetFrame::isGeneralizedTxSet() const
{
    return mIsGeneralized;
}

bool
TxSetFrame::addTxsFromXdr(Application& app,
                          xdr::xvector<TransactionEnvelope> const& txs,
                          bool useBaseFee, std::optional<int64_t> baseFee,
                          Phase phase)
{
    auto& phaseTxs = mTxPhases.at(static_cast<int>(phase));
    size_t oldSize = phaseTxs.size();
    phaseTxs.reserve(oldSize + txs.size());

    LedgerTxn ltx(app.getLedgerTxnRoot(), false,
                  TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    for (auto const& env : txs)
    {
        auto tx = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), env);

        if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
        {
            tx->maybeComputeSorobanResourceFee(
                ledgerVersion,
                app.getLedgerManager().getSorobanNetworkConfig(ltx),
                app.getConfig());
        }
        // Phase should be consistent with the tx we're trying to add
        if ((tx->isSoroban() && phase != Phase::SOROBAN) ||
            (!tx->isSoroban() && phase != Phase::CLASSIC))
        {
            return false;
        }

        phaseTxs.push_back(tx);
        if (useBaseFee)
        {
            getInclusionFeeMap(phase)[tx] = baseFee;
        }
    }
    return std::is_sorted(phaseTxs.begin() + oldSize, phaseTxs.end(),
                          &TxSetUtils::hashTxSorter);
}

void
TxSetFrame::applySurgePricing(Application& app)
{
    ZoneScoped;

    if (empty())
    {
        for (int i = 0; i < mFeesComputed.size(); ++i)
        {
            mFeesComputed[i] = true;
        }
        return;
    }

    auto const& lclHeader =
        app.getLedgerManager().getLastClosedLedgerHeader().header;

    releaseAssert(mTxPhases.size() <=
                  static_cast<int>(TxSetFrame::Phase::PHASE_COUNT));
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        TxSetFrame::Phase phaseType = static_cast<TxSetFrame::Phase>(i);
        auto& phase = mTxPhases[i];
        auto actTxQueues = TxSetUtils::buildAccountTxQueues(phase);

        if (phaseType == TxSetFrame::Phase::CLASSIC)
        {
            uint32_t maxOps = static_cast<uint32_t>(
                app.getLedgerManager().getLastMaxTxSetSizeOps());
            std::optional<uint32_t> dexOpsLimit;
            if (isGeneralizedTxSet())
            {
                // DEX operations limit implies that DEX transactions should
                // compete with each other in in a separate fee lane, which is
                // only possible with generalized tx set.
                dexOpsLimit = app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET;
            }

            auto surgePricingLaneConfig =
                std::make_shared<DexLimitingLaneConfig>(maxOps, dexOpsLimit);

            std::vector<bool> hadTxNotFittingLane;
            auto includedTxs =
                SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
                    std::vector<TxStackPtr>(actTxQueues.begin(),
                                            actTxQueues.end()),
                    surgePricingLaneConfig, hadTxNotFittingLane);

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
                computeTxFees(TxSetFrame::Phase::CLASSIC, lclHeader,
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
            releaseAssert(phaseType == TxSetFrame::Phase::SOROBAN);

            LedgerTxn ltx(app.getLedgerTxnRoot(), false,
                          TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);

            auto limits = app.getLedgerManager().maxLedgerResources(
                /* isSoroban */ true, ltx);

            auto surgePricingLaneConfig =
                std::make_shared<SorobanGenericLaneConfig>(limits);

            std::vector<bool> hadTxNotFittingLane;
            auto includedTxs =
                SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
                    std::vector<TxStackPtr>(actTxQueues.begin(),
                                            actTxQueues.end()),
                    surgePricingLaneConfig, hadTxNotFittingLane);

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

        releaseAssert(mFeesComputed[i]);
    }
}

void
TxSetFrame::computeContentsHash()
{
    ZoneScoped;
    releaseAssert(!mHash);
    if (!isGeneralizedTxSet())
    {
        TransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        mHash = computeNonGenericTxSetContentsHash(xdrTxSet);
    }
    else
    {
        GeneralizedTransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        mHash = xdrSha256(xdrTxSet);
    }
}

std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>&
TxSetFrame::getInclusionFeeMap(Phase phase) const
{
    if (phase == Phase::SOROBAN)
    {
        return mTxBaseFeeSoroban;
    }
    releaseAssert(phase == Phase::CLASSIC);
    return mTxBaseFeeClassic;
}

std::string
TxSetFrame::getPhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::CLASSIC:
        return "classic";
    case Phase::SOROBAN:
        return "soroban";
    default:
        throw std::runtime_error("Unknown phase");
    }
}
} // namespace stellar
