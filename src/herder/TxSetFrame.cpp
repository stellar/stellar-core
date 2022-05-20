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

namespace stellar
{
namespace
{

class InvalidTxSetFrame : public TxSetFrame
{
  public:
    InvalidTxSetFrame(bool isGeneralized, Hash const& hash)
        : TxSetFrame(isGeneralized, {}, TxSetFrame::Transactions{})
    {
        mHash = hash;
    }

    bool
    checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const override
    {
        return false;
    }
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
    auto bidIsFeeComponents =
        std::count_if(phase.v0Components().begin(), phase.v0Components().end(),
                      [](auto const& component) {
                          return !component.txsMaybeDiscountedFee().baseFee;
                      });
    if (bidIsFeeComponents > 1)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: more than 1 BID_IS_FEE component {}",
                   bidIsFeeComponents);
        return false;
    }

    bool componentsNormalized =
        std::is_sorted(phase.v0Components().begin(), phase.v0Components().end(),
                       [](auto const& c1, auto const& c2) {
                           if (!c1.txsMaybeDiscountedFee().baseFee ||
                               !c2.txsMaybeDiscountedFee().baseFee)
                           {
                               return !c1.txsMaybeDiscountedFee().baseFee;
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
}

TxSetFrame::TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
                       Transactions const& txs)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTxs(TxSetUtils::sortTxsInHashOrder(txs))
{
}

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
    return std::shared_ptr<TxSetFrame>(
        new TxSetFrame(false, previousLedgerHash, txs));
}

TxSetFrameConstPtr
TxSetFrame::makeEmpty(LedgerHeaderHistoryEntry const& lclHeader)
{
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
    if (!txSet->addTxsFromXdr(networkID, xdrTxSet.txs, false, 0))
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
            false, computeNonGenericTxSetContentsHash(
                       xdrTxSet.previousLedgerHash, txs));
    }
    txSet->computeContentsHash();
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(Hash const& networkID,
                         GeneralizedTransactionSet const& xdrTxSet)
{
    ZoneScoped;
    auto hash = xdrSha256(xdrTxSet);

    if (!validateTxSetXDRStructure(xdrTxSet))
    {
        return std::make_shared<InvalidTxSetFrame const>(true, hash);
    }

    std::shared_ptr<TxSetFrame> txSet(
        new TxSetFrame(true, xdrTxSet.v1TxSet().previousLedgerHash,
                       TxSetFrame::Transactions{}));
    // Mark fees as already computed as we read them from the XDR.
    txSet->mFeesComputed = true;
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
                    return std::make_shared<InvalidTxSetFrame const>(true,
                                                                     hash);
                }
                break;
            }
        }
    }
    txSet->mHash = hash;
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

                CLOG_DEBUG(Herder, "Got bad txSet: {} has too low base fee {}",
                           hexAbbrev(mPreviousLedgerHash), *fee);
                return false;
            }
            // Here we validate the fee bid in relation to the respective bid
            // from this tx set. The tx frame itself currently validates bid vs
            // baseFee in the ledger header, which is redundant for generalized
            // tx set, but relevant for other tx frame uses.
            if (tx->getFeeBid() < tx->getMinFee(lcl.header, fee))
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

    if (std::adjacent_find(mTxs.begin(), mTxs.end(),
                           [](auto const& lhs, auto const& rhs) {
                               return lhs->getFullHash() == rhs->getFullHash();
                           }) != mTxs.end())
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {} has duplicate transactions",
                   hexAbbrev(mPreviousLedgerHash));
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
    releaseAssert(isGeneralizedTxSet());
    if (mEncodedSize)
    {
        return *mEncodedSize;
    }
    ZoneScoped;
    GeneralizedTransactionSet encoded;
    toXDR(encoded);
    mEncodedSize = xdr::xdr_size(encoded);
    return *mEncodedSize;
}

void
TxSetFrame::computeTxFees(LedgerHeader const& lclHeader) const
{
    ZoneScoped;
    int64_t baseFee = lclHeader.baseFee;
    if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                  ProtocolVersion::V_11))
    {
        size_t ops = 0;
        int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
        auto rounding =
            protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                      GENERALIZED_TX_SET_PROTOCOL_VERSION)
                ? Rounding::ROUND_DOWN
                : Rounding::ROUND_UP;
        for (auto& txPtr : mTxs)
        {
            auto txOps = txPtr->getNumOperations();
            ops += txOps;
            int64_t txBaseFee = bigDivideOrThrow(
                txPtr->getFeeBid(), 1, static_cast<int64_t>(txOps), rounding);
            lowBaseFee = std::min(lowBaseFee, txBaseFee);
        }
        // if surge pricing was in action, use the lowest base fee bid from the
        // transaction set
        size_t surgeOpsCutoff = 0;
        if (lclHeader.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lclHeader.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (ops > surgeOpsCutoff)
        {
            baseFee = lowBaseFee;
        }
    }
    mFeesComputed = true;
    // Currently we apply the same base fee to all the transactions.
    for (auto const& tx : mTxs)
    {
        mTxBaseFee[tx] = baseFee;
    }
}

std::optional<int64_t>
TxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                         LedgerHeader const& lclHeader) const
{
    if (!mFeesComputed)
    {
        releaseAssert(!isGeneralizedTxSet());
        computeTxFees(lclHeader);
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

void
TxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    txSet.txs.resize(xdr::size32(mTxs.size()));
    for (unsigned int n = 0; n < mTxs.size(); n++)
    {
        txSet.txs[n] = mTxs[n]->getEnvelope();
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

    for (auto const& tx : mTxs)
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
                          bool useBaseFee, std::optional<uint32_t> baseFee)
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
TxSetFrame::surgePricingFilter(uint32_t opsLeft)
{
    ZoneScoped;
    auto curSizeOps = sizeOp();
    if (curSizeOps <= opsLeft)
    {
        return;
    }
    CLOG_WARNING(Herder, "surge pricing in effect! {} > {}", curSizeOps,
                 opsLeft);

    auto actTxQueueMap = TxSetUtils::buildAccountTxQueues(mTxs);

    std::priority_queue<TxSetFrame::AccountTransactionQueue*,
                        std::vector<TxSetFrame::AccountTransactionQueue*>,
                        SurgeCompare>
        surgeQueue;

    for (auto& am : actTxQueueMap)
    {
        surgeQueue.push(&am.second);
    }

    TxSetFrame::Transactions filteredTxs;
    filteredTxs.reserve(sizeTx());
    while (opsLeft > 0 && !surgeQueue.empty())
    {
        auto cur = surgeQueue.top();
        surgeQueue.pop();
        // inspect the top candidate queue
        auto& curTopTx = cur->front();
        auto opsCount = curTopTx->getNumOperations();
        if (opsCount <= opsLeft)
        {
            // pop from this one
            filteredTxs.emplace_back(curTopTx);
            cur->pop_front();
            opsLeft -= opsCount;
            // if there are more transactions, put it back
            if (!cur->empty())
            {
                surgeQueue.push(cur);
            }
        }
        else
        {
            // drop this transaction -> we need to drop the others
            cur->clear();
        }
    }
    mTxs = filteredTxs;
}

void
TxSetFrame::computeContentsHash()
{
    ZoneScoped;
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
