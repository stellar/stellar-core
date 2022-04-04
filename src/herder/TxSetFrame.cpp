// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetFrame.h"
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

using namespace std;

TxSetFrame::TxSetFrame(Hash const& previousLedgerHash, uint32_t ledgerVersion)
    : mHash(std::nullopt)
    , mValid(std::nullopt)
    , mPreviousLedgerHash(previousLedgerHash)
    , mGeneralized(protocolVersionStartsFrom(
          ledgerVersion, GENERALIZED_TX_SET_PROTOCOL_VERSION))
{
}

TxSetFrame::TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet)
    : mHash(std::nullopt), mValid(std::nullopt)
{
    ZoneScoped;
    addTxs(networkID, xdrSet.txs, /* isDiscounted= */ true);
    mPreviousLedgerHash = xdrSet.previousLedgerHash;
}

TxSetFrame::TxSetFrame(Hash const& networkID,
                       GeneralizedTransactionSet const& xdrSet)
    : mHash(std::nullopt), mValid(std::nullopt), mGeneralized(true)
{
    ZoneScoped;
    auto const& txSet = xdrSet.v1TxSet();

    auto const& phases = txSet.phases;
    for (auto const& phase : phases)
    {
        auto const& components = phase.v0Components();
        for (auto const& component : components)
        {
            switch (component.type())
            {
            case TXSET_COMP_TXS_BID_IS_FEE:
                addTxs(networkID, component.txsBidIsFee(),
                       /* isDiscounted= */ false);
                break;
            case TXSET_COMP_TXS_DISCOUNTED_FEE:
                addTxs(networkID, component.txsDiscountedFee().txs,
                       /* isDiscounted= */ true);
                mBaseFee = component.txsDiscountedFee().baseFee;
                break;
            }
        }
    }
    mPreviousLedgerHash = txSet.previousLedgerHash;
}

static bool
HashTxSorter(TransactionFrameBasePtr const& tx1,
             TransactionFrameBasePtr const& tx2)
{
    // need to use the hash of whole tx here since multiple txs could have
    // the same Contents
    return tx1->getFullHash() < tx2->getFullHash();
}

// order the txset correctly
// must take into account multiple tx from same account
void
TxSetFrame::sortForHash()
{
    ZoneScoped;
    std::sort(mTransactions.begin(), mTransactions.end(), HashTxSorter);
    mHash.reset();
    mValid.reset();
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

static bool
SeqSorter(TransactionFrameBasePtr const& tx1,
          TransactionFrameBasePtr const& tx2)
{
    return tx1->getSeqNum() < tx2->getSeqNum();
}

/*
    Build a list of transaction ready to be applied to the last closed ledger,
    based on the transaction set.

    The order satisfies:
    * transactions for an account are sorted by sequence number (ascending)
    * the order between accounts is randomized
*/
std::vector<TransactionFrameBasePtr>
TxSetFrame::sortForApply()
{
    ZoneScoped;
    auto txQueues = buildAccountTxQueues();

    // build txBatches
    // txBatches i-th element contains each i-th transaction for accounts with a
    // transaction in the transaction set
    std::list<std::deque<TransactionFrameBasePtr>> txBatches;

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

    vector<TransactionFrameBasePtr> retList;
    retList.reserve(mTransactions.size());
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

        auto& top1 = tx1->front();
        auto& top2 = tx2->front();

        auto cmp3 = feeRate3WayCompare(top1, top2);

        if (cmp3 != 0)
        {
            return cmp3 < 0;
        }
        // use hash of transaction as a tie breaker
        return lessThanXored(top1->getFullHash(), top2->getFullHash(), mSeed);
    }
};

UnorderedMap<AccountID, TxSetFrame::AccountTransactionQueue>
TxSetFrame::buildAccountTxQueues()
{
    ZoneScoped;
    UnorderedMap<AccountID, AccountTransactionQueue> actTxQueueMap;
    for (auto& tx : mTransactions)
    {
        auto id = tx->getSourceID();
        auto it = actTxQueueMap.find(id);
        if (it == actTxQueueMap.end())
        {
            auto d = std::make_pair(id, AccountTransactionQueue{});
            auto r = actTxQueueMap.insert(d);
            it = r.first;
        }
        it->second.emplace_back(tx);
    }

    for (auto& am : actTxQueueMap)
    {
        // sort each in sequence number order
        std::sort(am.second.begin(), am.second.end(), SeqSorter);
    }
    return actTxQueueMap;
}

void
TxSetFrame::addTxs(Hash const& networkID,
                   xdr::xvector<TransactionEnvelope> const& txs,
                   bool isDiscounted)
{
    for (auto const& env : txs)
    {
        auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, env,
                                                                isDiscounted);
        mTransactions.push_back(tx);
    }
}

void
TxSetFrame::surgePricingFilter(Application& app)
{
    ZoneScoped;
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto header = ltx.loadHeader();

    bool maxIsOps = protocolVersionStartsFrom(header.current().ledgerVersion,
                                              ProtocolVersion::V_11);

    size_t opsLeft = app.getLedgerManager().getLastMaxTxSetSizeOps();

    auto curSizeOps = maxIsOps ? sizeOp() : (sizeTx() * MAX_OPS_PER_TX);
    if (curSizeOps > opsLeft)
    {
        CLOG_WARNING(Herder, "surge pricing in effect! {} > {}", curSizeOps,
                     opsLeft);

        auto actTxQueueMap = buildAccountTxQueues();

        std::priority_queue<AccountTransactionQueue*,
                            std::vector<AccountTransactionQueue*>, SurgeCompare>
            surgeQueue;

        for (auto& am : actTxQueueMap)
        {
            surgeQueue.push(&am.second);
        }

        std::vector<TransactionFrameBasePtr> updatedSet;
        updatedSet.reserve(mTransactions.size());
        while (opsLeft > 0 && !surgeQueue.empty())
        {
            auto cur = surgeQueue.top();
            surgeQueue.pop();
            // inspect the top candidate queue
            auto& curTopTx = cur->front();
            size_t opsCount =
                maxIsOps ? curTopTx->getNumOperations() : MAX_OPS_PER_TX;
            if (opsCount <= opsLeft)
            {
                // pop from this one
                updatedSet.emplace_back(curTopTx);
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
        mTransactions = std::move(updatedSet);
        sortForHash();
    }
}

bool
TxSetFrame::checkOrTrim(Application& app,
                        std::vector<TransactionFrameBasePtr>& trimmed,
                        bool justCheck, uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    LedgerTxn ltx(app.getLedgerTxnRoot());

    UnorderedMap<AccountID, int64_t> accountFeeMap;
    auto accountTxMap = buildAccountTxQueues();
    for (auto& kv : accountTxMap)
    {
        int64_t lastSeq = 0;
        auto iter = kv.second.begin();
        while (iter != kv.second.end())
        {
            auto tx = *iter;
            if (!tx->checkValid(ltx, lastSeq, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset))
            {
                if (justCheck)
                {
                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: {} tx invalid lastSeq:{} tx: {} "
                        "result: {}",
                        hexAbbrev(mPreviousLedgerHash), lastSeq,
                        xdr_to_string(tx->getEnvelope(), "TransactionEnvelope"),
                        tx->getResultCode());
                    return false;
                }
                trimmed.emplace_back(tx);
                removeTx(tx);
                iter = kv.second.erase(iter);
            }
            else
            {
                lastSeq = tx->getSeqNum();
                int64_t& accFee = accountFeeMap[tx->getFeeSourceID()];
                if (INT64_MAX - accFee < tx->getFeeBid())
                {
                    accFee = INT64_MAX;
                }
                else
                {
                    accFee += tx->getFeeBid();
                }
                ++iter;
            }
        }
    }

    auto header = ltx.loadHeader();
    for (auto& kv : accountTxMap)
    {
        auto iter = kv.second.begin();
        while (iter != kv.second.end())
        {
            auto tx = *iter;
            auto feeSource = stellar::loadAccount(ltx, tx->getFeeSourceID());
            auto totFee = accountFeeMap[tx->getFeeSourceID()];
            if (getAvailableBalance(header, feeSource) < totFee)
            {
                if (justCheck)
                {
                    CLOG_DEBUG(Herder,
                               "Got bad txSet: {} account can't pay fee tx: {}",
                               hexAbbrev(mPreviousLedgerHash),
                               xdr_to_string(tx->getEnvelope(),
                                             "TransactionEnvelope"));
                    return false;
                }
                while (iter != kv.second.end())
                {
                    trimmed.emplace_back(*iter);
                    removeTx(*iter);
                    ++iter;
                }
            }
            else
            {
                ++iter;
            }
        }
    }

    return true;
}

std::vector<TransactionFrameBasePtr>
TxSetFrame::trimInvalid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    std::vector<TransactionFrameBasePtr> trimmed;
    sortForHash();
    checkOrTrim(app, trimmed, false, lowerBoundCloseTimeOffset,
                upperBoundCloseTimeOffset);
    return trimmed;
}

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
TxSetFrame::checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                       uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    auto& lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    if (mValid && mValid->first == lcl.hash)
    {
        return mValid->second;
    }
    // Start by checking previousLedgerHash
    if (lcl.hash != mPreviousLedgerHash)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {}, expected {}",
                   hexAbbrev(mPreviousLedgerHash), hexAbbrev(lcl.hash));
        mValid = std::make_optional<std::pair<Hash, bool>>(lcl.hash, false);
        return false;
    }

    if (this->size(lcl.header) > lcl.header.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many txs {} > {}",
                   this->size(lcl.header), lcl.header.maxTxSetSize);
        mValid = std::make_optional<std::pair<Hash, bool>>(lcl.hash, false);
        return false;
    }

    if (!std::is_sorted(mTransactions.begin(), mTransactions.end(),
                        [](auto const& lhs, auto const& rhs) {
                            return lhs->getFullHash() < rhs->getFullHash();
                        }))
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {} not sorted correctly",
                   hexAbbrev(mPreviousLedgerHash));
        mValid = std::make_optional<std::pair<Hash, bool>>(lcl.hash, false);
        return false;
    }

    std::vector<TransactionFrameBasePtr> trimmed;
    bool valid = checkOrTrim(app, trimmed, true, lowerBoundCloseTimeOffset,
                             upperBoundCloseTimeOffset);
    mValid = std::make_optional<std::pair<Hash, bool>>(lcl.hash, valid);
    return valid;
}

void
TxSetFrame::removeTx(TransactionFrameBasePtr tx)
{
    auto it = std::find(mTransactions.begin(), mTransactions.end(), tx);
    if (it != mTransactions.end())
    {
        mTransactions.erase(it);
    }
    mHash.reset();
    mValid.reset();
    mBaseFee.reset();
}

void
TxSetFrame::add(TransactionFrameBasePtr tx)
{
    mTransactions.push_back(tx);
    mHash.reset();
    mValid.reset();
    mBaseFee.reset();
}

Hash const&
TxSetFrame::getContentsHash()
{
    ZoneScoped;
    if (mHash)
    {
        return *mHash;
    }

    sortForHash();
    SHA256 hasher;
    if (isGeneralizedTxSet())
    {
        GeneralizedTransactionSet txSet;
        toXDR(txSet);
        hasher.add(xdr::xdr_to_opaque(txSet));
    }
    else
    {
        hasher.add(mPreviousLedgerHash);
        for (unsigned int n = 0; n < mTransactions.size(); n++)
        {
            hasher.add(xdr::xdr_to_opaque(mTransactions[n]->getEnvelope()));
        }
    }

    mHash = std::make_optional<Hash>(hasher.finish());
    return *mHash;
}

Hash&
TxSetFrame::previousLedgerHash()
{
    // Handing out a mutable reference means the caller might
    // be mutating, so we treat this as an invalidation event.
    mHash.reset();
    mValid.reset();
    mBaseFee.reset();
    return mPreviousLedgerHash;
}

Hash const&
TxSetFrame::previousLedgerHash() const
{
    return mPreviousLedgerHash;
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
    return std::accumulate(mTransactions.begin(), mTransactions.end(),
                           size_t(0),
                           [](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

int64_t
TxSetFrame::getBaseFee(LedgerHeader const& lh) const
{
    if (mBaseFee)
    {
        return *mBaseFee;
    }
    int64_t baseFee = lh.baseFee;
    if (protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11))
    {
        size_t ops = 0;
        int64_t lowBaseFee = std::numeric_limits<int64_t>::max();

        for (auto& txPtr : mTransactions)
        {
            if (mGeneralized && !txPtr->isDiscounted())
            {
                continue;
            }
            auto txOps = txPtr->getNumOperations();
            ops += txOps;
            int64_t txBaseFee = bigDivideOrThrow(txPtr->getFeeBid(), 1,
                                                 static_cast<int64_t>(txOps),
                                                 Rounding::ROUND_UP);
            lowBaseFee = std::min(lowBaseFee, txBaseFee);
        }
        // if surge pricing was in action, use the lowest base fee bid from the
        // transaction set
        size_t surgeOpsCutoff = 0;
        if (lh.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lh.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (ops > surgeOpsCutoff)
        {
            baseFee = lowBaseFee;
        }
    }
    mBaseFee = baseFee;
    return baseFee;
}

bool
TxSetFrame::isDiscountedTransaction(TransactionFrameBasePtr tx) const
{
    // Currently we preserve the old behavior and discount all the
    // transactions.
    return true;
}

int64_t
TxSetFrame::getTotalFees(LedgerHeader const& lh) const
{
    ZoneScoped;
    releaseAssert(!mGeneralized);
    auto baseFee = getBaseFee(lh);
    return std::accumulate(mTransactions.begin(), mTransactions.end(),
                           int64_t(0),
                           [&](int64_t t, TransactionFrameBasePtr const& tx) {
                               return t + tx->getFee(lh, baseFee, true);
                           });
}

int64_t
TxSetFrame::getTotalBids() const
{
    ZoneScoped;
    releaseAssert(mGeneralized);
    return std::accumulate(mTransactions.begin(), mTransactions.end(),
                           int64_t(0),
                           [&](int64_t t, TransactionFrameBasePtr const& tx) {
                               return t + tx->getFeeBid();
                           });
}

void
TxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    releaseAssert(std::is_sorted(mTransactions.begin(), mTransactions.end(),
                                 HashTxSorter));
    txSet.txs.resize(xdr::size32(mTransactions.size()));
    for (unsigned int n = 0; n < mTransactions.size(); n++)
    {
        txSet.txs[n] = mTransactions[n]->getEnvelope();
    }
    txSet.previousLedgerHash = mPreviousLedgerHash;
}

void
TxSetFrame::toXDR(GeneralizedTransactionSet& generalizedTxSet) const
{
    ZoneScoped;
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(mBaseFee);
    releaseAssert(std::is_sorted(mTransactions.begin(), mTransactions.end(),
                                 HashTxSorter));
    generalizedTxSet.v(1);
    // The following code assumes that only a single phase exists.
    auto& phase =
        generalizedTxSet.v1TxSet().phases.emplace_back().v0Components();
    phase.emplace_back(TXSET_COMP_TXS_DISCOUNTED_FEE);
    phase.emplace_back(TXSET_COMP_TXS_BID_IS_FEE);
    auto& discountedFeeComponent = phase[0].txsDiscountedFee();
    discountedFeeComponent.baseFee = *mBaseFee;
    auto& discountedTxs = discountedFeeComponent.txs;
    auto& bidIsFeeTxs = phase[1].txsBidIsFee();

    discountedTxs.reserve(mTransactions.size());
    for (auto const& tx : mTransactions)
    {
        if (tx->isDiscounted())
        {
            discountedTxs.emplace_back(tx->getEnvelope());
        }
        else
        {
            bidIsFeeTxs.emplace_back(tx->getEnvelope());
        }
    }

    generalizedTxSet.v1TxSet().previousLedgerHash = mPreviousLedgerHash;
}

bool
TxSetFrame::isGeneralizedTxSet() const
{
    return mGeneralized;
}

} // namespace stellar
