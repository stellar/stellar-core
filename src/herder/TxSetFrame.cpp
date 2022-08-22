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
struct SurgeCompare
{
    Hash mSeed;
    SurgeCompare() : mSeed(HashUtils::random())
    {
    }
    // return true if tx1 < tx2
    bool
    operator()(TxSetFrame::AccountTransactionQueue const* tx1,
               TxSetFrame::AccountTransactionQueue const* tx2) const
    {
        if (tx1 == nullptr || tx1->empty())
        {
            return tx2 ? !tx2->empty() : false;
        }
        if (tx2 == nullptr || tx2->empty())
        {
            return false;
        }
        auto const& top1 = tx1->front();
        auto const& top2 = tx2->front();
        auto cmp3 = feeRate3WayCompare(*top1, *top2);
        if (cmp3 != 0)
        {
            return cmp3 < 0;
        }
        // use hash of transaction as a tie breaker
        return lessThanXored(top1->getFullHash(), top2->getFullHash(), mSeed);
    }
};
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
computeNonGenericTxSetContentsHash(Hash const& previousLedgerHash,
                                   TxSetFrame::Transactions const& txs)
{
    ZoneScoped;
    SHA256 hasher;
    hasher.add(previousLedgerHash);
    for (unsigned int n = 0; n < txs.size(); n++)
    {
        hasher.add(xdr::xdr_to_opaque(txs[n]->getEnvelope()));
    }
    return hasher.finish();
}
} // namespace
TxSetFrame::TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
                       Transactions const& txs)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTxs(TxSetUtils::sortTxsInHashOrder(txs))
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
    txSet->surgePricingFilter(app.getLedgerManager().getLastMaxTxSetSizeOps());
    txSet->computeTxFees(lclHeader.header);
    txSet->computeContentsHash();
    if (!txSet->checkValid(app, lowerBoundCloseTimeOffset,
                           upperBoundCloseTimeOffset))
    {
        throw std::runtime_error("Created invalid tx set frame");
    }
    return txSet;
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
    txSet->computeTxFees(lclHeader.header);
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
        Transactions txs;
        std::transform(xdrTxSet.txs.begin(), xdrTxSet.txs.end(),
                       std::back_inserter(txs), [&networkID](auto const& env) {
                           return TransactionFrameBase::makeTransactionFromWire(
                               networkID, env);
                       });
        return std::make_shared<InvalidTxSetFrame const>(
            xdrTxSet,
            computeNonGenericTxSetContentsHash(xdrTxSet.previousLedgerHash,
                                               txs),
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
    // Every component is sorted, but we do not merge-sort them during the
    // insertion and just sort them after everything is added. Currently this
    // doesn't really matter, but it's needed to maintain the
    // `getTxsInHashOrder` invariant.
    txSet->mTxs = TxSetUtils::sortTxsInHashOrder(txSet->mTxs);
    return txSet;
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
TxSetFrame::getTxsInHashOrder() const
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
    std::list<AccountTransactionQueue> txBatches;
    while (!txQueues.empty())
    {
        txBatches.emplace_back();
        auto& curBatch = txBatches.back();
        // go over all users that still have transactions
        for (auto it = txQueues.begin(); it != txQueues.end();)
        {
            auto& h = it->second.front();
            curBatch.emplace_back(h);
            it->second.pop_front();
            if (it->second.empty())
            {
                // done with that user
                it = txQueues.erase(it);
            }
            else
            {
                it++;
            }
        }
    }
    Transactions txsInApplyOrder;
    txsInApplyOrder.reserve(mTxs.size());
    for (auto& batch : txBatches)
    {
        // randomize each batch using the hash of the transaction set
        // as a way to randomize even more
        ApplyTxSorter s(getContentsHash());
        std::sort(batch.begin(), batch.end(), s);
        for (auto const& tx : batch)
        {
            txsInApplyOrder.push_back(tx);
        }
    }
    return txsInApplyOrder;
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

    if (!std::is_sorted(mTxs.begin(), mTxs.end(), &TxSetUtils::hashTxSorter))
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {} is not in hash order",
                   hexAbbrev(mPreviousLedgerHash));
        return false;
    }

    if (std::adjacent_find(mTxs.begin(), mTxs.end(),
                           [](auto const& lhs, auto const& rhs) {
                               return lhs->getFullHash() == rhs->getFullHash();
	@@ -840,7 +847,7 @@ TxSetFrame::surgePricingFilter(uint32_t opsLeft)
            cur->clear();
        }
    }
    mTxs = TxSetUtils::sortTxsInHashOrder(filteredTxs);
}

void
TxSetFrame::computeContentsHash()
{
    ZoneScoped;
    releaseAssert(!mHash);
    if (!isGeneralizedTxSet())
    {
        mHash = computeNonGenericTxSetContentsHash(mPreviousLedgerHash, mTxs);
    }
    else
    {
        GeneralizedTransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        mHash = xdrSha256(xdrTxSet);
    }
}
} // namespace stellar
