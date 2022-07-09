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
                     TxSetFrame::Transactions{})
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
    if (txSetV1.phases.size() != 1)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: exactly 1 phase is expected, got {}",
                   txSetV1.phases.size());
        return false;
    }

    auto const& phase = txSetV1.phases[0];
    if (phase.v() != 0)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: unsupported phase version {}",
                   phase.v());
        return false;
    }

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

int64_t
computePerOpFee(TransactionFrameBase const& tx, uint32_t ledgerVersion)
{
    auto rounding = protocolVersionStartsFrom(
                        ledgerVersion, GENERALIZED_TX_SET_PROTOCOL_VERSION)
                        ? Rounding::ROUND_DOWN
                        : Rounding::ROUND_UP;
    auto txOps = tx.getNumOperations();
    return bigDivideOrThrow(tx.getFeeBid(), 1, static_cast<int64_t>(txOps),
                            rounding);
}

} // namespace

TxSetFrame::TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
                       Transactions const& txs)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTxs(txs)
{
}

#ifdef BUILD_TESTS
bool
TxSetFrame::checkValidStructure() const
{
    return true;
}
#endif

TxSetFrame::TxSetFrame(LedgerHeaderHistoryEntry const& lclHeader,
                       Transactions const& txs)
    : TxSetFrame(protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                           GENERALIZED_TX_SET_PROTOCOL_VERSION),
                 lclHeader.hash, txs)
{
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxSetFrame::Transactions const& txs,
                                 Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 TxSetFrame::Transactions* invalidTxs)
{
    TxSetFrame::Transactions unusedInvalidTxs;
    if (!invalidTxs)
    {
        invalidTxs = &unusedInvalidTxs;
    }
    auto validTxs =
        TxSetUtils::trimInvalid(txs, app, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset, *invalidTxs);
    auto const& lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    // We can't use `std::make_shared` here as the constructors are protected.
    // This may cause leaks in case of exceptions, so keep the constructors
    // simple and exception-safe.
    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(lclHeader, validTxs));
    txSet->applySurgePricing(app);

    // Do the roundtrip through XDR to ensure we never build an incorrect tx set
    // for nomination.
    TxSetFrameConstPtr outputTxSet;
    if (txSet->isGeneralizedTxSet())
    {
        GeneralizedTransactionSet xdrTxSet;
        txSet->toXDR(xdrTxSet);
        outputTxSet = TxSetFrame::makeFromWire(app.getNetworkID(), xdrTxSet);
    }
    else
    {
        TransactionSet xdrTxSet;
        txSet->toXDR(xdrTxSet);
        outputTxSet = TxSetFrame::makeFromWire(app.getNetworkID(), xdrTxSet);
    }
    // Make sure no transactions were lost during the roundtrip and the output
    // tx set is valid.
    if (txSet->getTxs().size() != outputTxSet->getTxs().size() ||
        !outputTxSet->checkValid(app, lowerBoundCloseTimeOffset,
                                 upperBoundCloseTimeOffset))
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
        new TxSetFrame(false, previousLedgerHash, txs));
}

TxSetFrameConstPtr
TxSetFrame::makeEmpty(LedgerHeaderHistoryEntry const& lclHeader)
{
    // We can't use `std::make_shared` here as the constructors are protected.
    // This may cause leaks in case of exceptions, so keep the constructors
    // simple and exception-safe.
    std::shared_ptr<TxSetFrame> txSet(
        new TxSetFrame(lclHeader, TxSetFrame::Transactions{}));
    txSet->mFeesComputed = true;
    txSet->computeContentsHash();
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(Hash const& networkID, TransactionSet const& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(
        false, xdrTxSet.previousLedgerHash, TxSetFrame::Transactions{}));
    size_t encodedSize = xdr::xdr_argpack_size(xdrTxSet);
    if (!txSet->addTxsFromXdr(networkID, xdrTxSet.txs, false, std::nullopt))
    {
        CLOG_DEBUG(Herder, "Got bad txSet: transactions are not "
                           "ordered correctly");
        return std::make_shared<InvalidTxSetFrame const>(
            xdrTxSet, computeNonGenericTxSetContentsHash(xdrTxSet),
            encodedSize);
    }
    txSet->computeContentsHash();
    txSet->mEncodedSize = encodedSize;
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(Hash const& networkID,
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

    std::shared_ptr<TxSetFrame> txSet(
        new TxSetFrame(true, xdrTxSet.v1TxSet().previousLedgerHash,
                       TxSetFrame::Transactions{}));
    // Mark fees as already computed as we read them from the XDR.
    txSet->mFeesComputed = true;
    txSet->mHash = hash;
    auto const& phases = xdrTxSet.v1TxSet().phases;
    for (auto const& phase : phases)
    {
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
                if (!txSet->addTxsFromXdr(networkID,
                                          component.txsMaybeDiscountedFee().txs,
                                          true, baseFee))
                {
                    CLOG_DEBUG(Herder, "Got bad txSet: transactions are not "
                                       "ordered correctly");
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
        cur = TxSetFrame::makeFromWire(app.getNetworkID(), storedSet.txSet());
    }
    else
    {
        cur = TxSetFrame::makeFromWire(app.getNetworkID(),
                                       storedSet.generalizedTxSet());
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
TxSetFrame::getTxs() const
{
    return mTxs;
}

TxSetFrame::Transactions
TxSetFrame::getTxsInApplyOrder() const
{
    ZoneScoped;
    auto txQueues = TxSetUtils::buildAccountTxQueues(mTxs);

    // build txBatches
    // txBatches i-th element contains each i-th transaction for accounts with a
    // transaction in the transaction set
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

    std::vector<TransactionFrameBasePtr> retList;
    retList.reserve(mTxs.size());
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
        releaseAssert(mFeesComputed);
        for (auto const& [tx, fee] : mTxBaseFee)
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
            if (tx->getFeeBid() < getMinFee(*tx, lcl.header, fee))
            {
                CLOG_DEBUG(
                    Herder,
                    "Got bad txSet: {} has tx with fee bid lower than base fee",
                    hexAbbrev(mPreviousLedgerHash));
                return false;
            }
        }
    }

    if (this->size(lcl.header) > lcl.header.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many txs {} > {}",
                   this->size(lcl.header), lcl.header.maxTxSetSize);
        return false;
    }

    return TxSetUtils::getInvalidTxList(mTxs, app, lowerBoundCloseTimeOffset,
                                        upperBoundCloseTimeOffset, true)
        .empty();
}

size_t
TxSetFrame::size(LedgerHeader const& lh) const
{
    return protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11)
               ? sizeOp()
               : sizeTx();
}

size_t
TxSetFrame::sizeOp() const
{
    ZoneScoped;
    return std::accumulate(mTxs.begin(), mTxs.end(), size_t(0),
                           [](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
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
    for (auto& txPtr : mTxs)
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
    auto ledgerVersion = lclHeader.ledgerVersion;
    int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
    for (auto& txPtr : mTxs)
    {
        int64_t txBaseFee = computePerOpFee(*txPtr, ledgerVersion);
        lowBaseFee = std::min(lowBaseFee, txBaseFee);
    }
    computeTxFees(lclHeader, lowBaseFee,
                  /* enableLogging */ false);
}

void
TxSetFrame::computeTxFees(LedgerHeader const& lclHeader, int64_t lowestBaseFee,
                          bool enableLogging) const
{
    ZoneScoped;
    releaseAssert(!mFeesComputed);
    int64_t baseFee = lclHeader.baseFee;

    if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                  ProtocolVersion::V_11))
    {
        size_t surgeOpsCutoff = 0;
        if (lclHeader.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lclHeader.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (sizeOp() > surgeOpsCutoff)
        {
            baseFee = lowestBaseFee;
            if (enableLogging)
            {
                CLOG_WARNING(Herder, "surge pricing in effect! {} > {}",
                             sizeOp(), surgeOpsCutoff);
            }
        }
    }

    for (auto const& tx : mTxs)
    {
        mTxBaseFee[tx] = baseFee;
    }
    mFeesComputed = true;
}

void
TxSetFrame::computeTxFees(LedgerHeader const& ledgerHeader,
                          SurgePricingLaneConfig const& surgePricingConfig,
                          std::vector<int64_t> const& lowestLaneFee,
                          std::vector<bool> const& hadTxNotFittingLane)
{
    releaseAssert(!mFeesComputed);
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
                "surge pricing for '{}' lane is in effect with base fee={}",
                lane == SurgePricingPriorityQueue::GENERIC_LANE ? "generic"
                                                                : "DEX",
                laneBaseFee[lane]);
        }
    }

    for (auto const& tx : mTxs)
    {
        mTxBaseFee[tx] = laneBaseFee[surgePricingConfig.getLane(*tx)];
    }
    mFeesComputed = true;
}

std::optional<int64_t>
TxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                         LedgerHeader const& lclHeader) const
{
    if (!mFeesComputed)
    {
        releaseAssert(!isGeneralizedTxSet());
        computeTxFeesForNonGeneralizedSet(lclHeader);
    }
    auto it = mTxBaseFee.find(tx);
    if (it == mTxBaseFee.end())
    {
        throw std::runtime_error("Transaction not found in tx set");
    }
    return it->second;
}

int64_t
TxSetFrame::getTotalFees(LedgerHeader const& lh) const
{
    ZoneScoped;
    return std::accumulate(mTxs.begin(), mTxs.end(), int64_t(0),
                           [&](int64_t t, TransactionFrameBasePtr const& tx) {
                               return t + tx->getFee(lh, getTxBaseFee(tx, lh),
                                                     true);
                           });
}

int64_t
TxSetFrame::getTotalBids() const
{
    ZoneScoped;
    return std::accumulate(mTxs.begin(), mTxs.end(), int64_t(0),
                           [&](int64_t t, TransactionFrameBasePtr const& tx) {
                               return t + tx->getFeeBid();
                           });
}

std::string
TxSetFrame::summary() const
{
    if (mTxs.empty())
    {
        return "empty tx set";
    }
    if (isGeneralizedTxSet())
    {
        std::map<std::optional<int64_t>, std::pair<int, int>> componentStats;
        for (auto const& [tx, fee] : mTxBaseFee)
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
    }
    else
    {
        return fmt::format(FMT_STRING("txs:{}, ops:{}, base_fee:{}"), sizeTx(),
                           sizeOp(), *mTxBaseFee.begin()->second);
    }
}

void
TxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    txSet.txs.resize(xdr::size32(mTxs.size()));
    auto sortedTxs = TxSetUtils::sortTxsInHashOrder(mTxs);
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
    releaseAssert(mFeesComputed);

    generalizedTxSet.v(1);
    // The following code assumes that only a single phase exists.
    auto& phase =
        generalizedTxSet.v1TxSet().phases.emplace_back().v0Components();

    std::map<std::optional<int64_t>, size_t> feeTxCount;
    for (auto const& [tx, fee] : mTxBaseFee)
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
    auto sortedTxs = TxSetUtils::sortTxsInHashOrder(mTxs);
    for (auto const& tx : sortedTxs)
    {
        componentPerBid[mTxBaseFee.find(tx)->second]->push_back(
            tx->getEnvelope());
    }

    generalizedTxSet.v1TxSet().previousLedgerHash = mPreviousLedgerHash;
}

bool
TxSetFrame::isGeneralizedTxSet() const
{
    return mIsGeneralized;
}

bool
TxSetFrame::addTxsFromXdr(Hash const& networkID,
                          xdr::xvector<TransactionEnvelope> const& txs,
                          bool useBaseFee, std::optional<int64_t> baseFee)
{
    size_t oldSize = mTxs.size();
    mTxs.reserve(oldSize + txs.size());
    for (auto const& env : txs)
    {
        auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, env);
        mTxs.push_back(tx);
        if (useBaseFee)
        {
            mTxBaseFee[tx] = baseFee;
        }
    }
    return std::is_sorted(mTxs.begin() + oldSize, mTxs.end(),
                          &TxSetUtils::hashTxSorter);
}

void
TxSetFrame::applySurgePricing(Application& app)
{
    ZoneScoped;

    if (mTxs.empty())
    {
        mFeesComputed = true;
        return;
    }

    uint32_t maxOps =
        static_cast<uint32_t>(app.getLedgerManager().getLastMaxTxSetSizeOps());
    auto const& lclHeader =
        app.getLedgerManager().getLastClosedLedgerHeader().header;
    std::optional<uint32_t> dexOpsLimit;
    if (isGeneralizedTxSet())
    {
        // DEX operations limit implies that DEX transactions should compete
        // with each other in in a separate fee lane, which is only possible
        // with generalized tx set.
        dexOpsLimit = app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET;
    }

    auto actTxQueues = TxSetUtils::buildAccountTxQueues(mTxs);
    auto surgePricingLaneConfig =
        std::make_shared<DexLimitingLaneConfig>(maxOps, dexOpsLimit);

    std::vector<bool> hadTxNotFittingLane;
    auto includedTxs = SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
        std::vector<TxStackPtr>(actTxQueues.begin(), actTxQueues.end()),
        surgePricingLaneConfig, hadTxNotFittingLane);

    size_t laneCount = surgePricingLaneConfig->getLaneOpsLimits().size();
    std::vector<int64_t> lowestLaneFee(laneCount,
                                       std::numeric_limits<int64_t>::max());
    for (auto const& tx : includedTxs)
    {
        size_t lane = surgePricingLaneConfig->getLane(*tx);
        auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
        lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
    }

    mTxs = includedTxs;
    if (isGeneralizedTxSet())
    {
        computeTxFees(lclHeader, *surgePricingLaneConfig, lowestLaneFee,
                      hadTxNotFittingLane);
    }
    else
    {
        computeTxFeesForNonGeneralizedSet(
            lclHeader, lowestLaneFee[SurgePricingPriorityQueue::GENERIC_LANE],
            /* enableLogging */ true);
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

} // namespace stellar
