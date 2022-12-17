// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TransactionQueue.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxQueueLimiter.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "test/TxTests.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/BitSet.h"
#include "util/GlobalChecks.h"
#include "util/HashOfHash.h"
#include "util/Math.h"
#include "util/ProtocolVersion.h"
#include "util/TarjanSCCCalculator.h"
#include "util/XDROperators.h"
#include "util/numeric128.h"

#include <Tracy.hpp>
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <numeric>
#include <optional>
#include <random>

namespace stellar
{
const uint64_t TransactionQueue::FEE_MULTIPLIER = 10;

std::array<const char*,
           static_cast<int>(TransactionQueue::AddResult::ADD_STATUS_COUNT)>
    TX_STATUS_STRING = std::array{"PENDING", "DUPLICATE", "ERROR",
                                  "TRY_AGAIN_LATER", "FILTERED"};

TransactionQueue::TransactionQueue(Application& app, uint32 pendingDepth,
                                   uint32 banDepth, uint32 poolLedgerMultiplier)
    : mApp(app)
    , mPendingDepth(pendingDepth)
    , mBannedTransactions(banDepth)
    , mLedgerVersion(app.getLedgerManager()
                         .getLastClosedLedgerHeader()
                         .header.ledgerVersion)
    , mBannedTransactionsCounter(
          app.getMetrics().NewCounter({"herder", "pending-txs", "banned"}))
    , mArbTxSeenCounter(
          app.getMetrics().NewCounter({"herder", "arb-tx", "seen"}))
    , mArbTxDroppedCounter(
          app.getMetrics().NewCounter({"herder", "arb-tx", "dropped"}))
    , mTransactionsDelay(
          app.getMetrics().NewTimer({"herder", "pending-txs", "delay"}))
    , mTransactionsSelfDelay(
          app.getMetrics().NewTimer({"herder", "pending-txs", "self-delay"}))
    , mBroadcastTimer(app)
{
    mTxQueueLimiter =
        std::make_unique<TxQueueLimiter>(poolLedgerMultiplier, app);
    for (uint32 i = 0; i < pendingDepth; i++)
    {
        mSizeByAge.emplace_back(&app.getMetrics().NewCounter(
            {"herder", "pending-txs", fmt::format(FMT_STRING("age{:d}"), i)}));
    }

    auto const& filteredTypes =
        app.getConfig().EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE;
    mFilteredTypes.insert(filteredTypes.begin(), filteredTypes.end());
    mBroadcastSeed =
        rand_uniform<uint64>(0, std::numeric_limits<uint64>::max());
    mBroadcastOpCarryover.resize(1);
}

TransactionQueue::~TransactionQueue()
{
    // empty destructor needed here due to the dependency on TxQueueLimiter
}

// returns true, if a transaction can be replaced by another
// `minFee` is set when returning false, and is the smallest fee
// that would allow replace by fee to succeed in this situation
static bool
canReplaceByFee(TransactionFrameBasePtr tx, TransactionFrameBasePtr oldTx,
                int64_t& minFee)
{
    int64_t newFee = tx->getFeeBid();
    uint32_t newNumOps = std::max<uint32_t>(1, tx->getNumOperations());
    int64_t oldFee = oldTx->getFeeBid();
    uint32_t oldNumOps = std::max<uint32_t>(1, oldTx->getNumOperations());

    // newFee / newNumOps >= FEE_MULTIPLIER * oldFee / oldNumOps
    // is equivalent to
    // newFee * oldNumOps >= FEE_MULTIPLIER * oldFee * newNumOps
    //
    // FEE_MULTIPLIER * v2 does not overflow uint128_t because fees are bounded
    // by INT64_MAX, while number of operations and FEE_MULTIPLIER are small.
    uint128_t v1 = bigMultiply(newFee, oldNumOps);
    uint128_t v2 = bigMultiply(oldFee, newNumOps);
    uint128_t minFeeN = v2 * TransactionQueue::FEE_MULTIPLIER;
    bool res = v1 >= minFeeN;
    if (!res)
    {
        if (!bigDivide128(minFee, minFeeN, int64_t(oldNumOps),
                          Rounding::ROUND_UP))
        {
            minFee = INT64_MAX;
        }
    }
    return res;
}

// This method will update iter to point to the tx with seqNum == seq if it is
// found. It also returns a bool that will be false if it determines seq cannot
// be added to the current queue, allowing the user of this function to make a
// decision early if desired.
static bool
findBySeq(int64_t seq, std::optional<SequenceNumber const> minSeqNum,
          TransactionQueue::TimestampedTransactions& transactions,
          TransactionQueue::TimestampedTransactions::iterator& iter)
{
    int64_t firstSeq = transactions.front().mTx->getSeqNum();
    int64_t lastSeq = transactions.back().mTx->getSeqNum();

    // check if seq is too low
    if (seq < firstSeq)
    {
        iter = transactions.end();
        return false;
    }

    // check if seq is new, and if it is, if minSeqNum would make it valid
    if (seq > lastSeq)
    {
        iter = transactions.end();
        return minSeqNum ? *minSeqNum <= lastSeq : seq == lastSeq + 1;
    }

    // by this point we're expecting to find an existing transaction, and if we
    // don't it's a gap that can't get filled
    iter = std::lower_bound(transactions.begin(), transactions.end(), seq,
                            [](TransactionQueue::TimestampedTx const& a,
                               int64_t b) { return a.mTx->getSeqNum() < b; });

    releaseAssert(iter == transactions.end() || iter->mTx->getSeqNum() >= seq);
    if (iter != transactions.end() && iter->mTx->getSeqNum() != seq)
    {
        // found a gap
        iter = transactions.end();
        return false;
    }
    return true;
}

static bool
findBySeq(TransactionFrameBasePtr tx,
          TransactionQueue::TimestampedTransactions& transactions,
          TransactionQueue::TimestampedTransactions::iterator& iter)
{
    return findBySeq(tx->getSeqNum(), tx->getMinSeqNum(), transactions, iter);
}

static bool
isDuplicateTx(TransactionFrameBasePtr oldTx, TransactionFrameBasePtr newTx)
{
    auto const& oldEnv = oldTx->getEnvelope();
    auto const& newEnv = newTx->getEnvelope();

    if (oldEnv.type() == newEnv.type())
    {
        return oldTx->getFullHash() == newTx->getFullHash();
    }
    else if (oldEnv.type() == ENVELOPE_TYPE_TX_FEE_BUMP)
    {
        auto oldFeeBump =
            std::static_pointer_cast<FeeBumpTransactionFrame>(oldTx);
        return oldFeeBump->getInnerFullHash() == newTx->getFullHash();
    }
    return false;
}

TransactionQueue::AddResult
TransactionQueue::canAdd(TransactionFrameBasePtr tx,
                         AccountStates::iterator& stateIter,
                         TimestampedTransactions::iterator& oldTxIter,
                         std::vector<std::pair<TxStackPtr, bool>>& txsToEvict)
{
    ZoneScoped;
    if (isBanned(tx->getFullHash()))
    {
        return TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER;
    }
    if (isFiltered(tx))
    {
        return TransactionQueue::AddResult::ADD_STATUS_FILTERED;
    }

    int64_t netFee = tx->getFeeBid();
    int64_t seqNum = 0;
    TransactionFrameBasePtr oldTx;

    stateIter = mAccountStates.find(tx->getSourceID());
    if (stateIter != mAccountStates.end())
    {
        auto& transactions = stateIter->second.mTransactions;
        oldTxIter = transactions.end();

        if (!transactions.empty())
        {
            if (tx->getEnvelope().type() != ENVELOPE_TYPE_TX_FEE_BUMP)
            {
                TimestampedTransactions::iterator iter;
                if (findBySeq(tx, transactions, iter) &&
                    iter != transactions.end() && isDuplicateTx(iter->mTx, tx))
                {
                    return TransactionQueue::AddResult::ADD_STATUS_DUPLICATE;
                }

                // By this point, there's already a tx in the queue for this
                // account, and only the tx with the lowest seqnum for an
                // account can have non-zero values for these fields.
                if (tx->getMinSeqAge() != 0 || tx->getMinSeqLedgerGap() != 0)
                {
                    return TransactionQueue::AddResult::
                        ADD_STATUS_TRY_AGAIN_LATER;
                }

                seqNum = transactions.back().mTx->getSeqNum();
            }
            else
            {
                if (!findBySeq(tx, transactions, oldTxIter))
                {
                    tx->getResult().result.code(txBAD_SEQ);
                    return TransactionQueue::AddResult::ADD_STATUS_ERROR;
                }

                if (oldTxIter != transactions.end())
                {
                    // Replace-by-fee logic
                    if (isDuplicateTx(oldTxIter->mTx, tx))
                    {
                        return TransactionQueue::AddResult::
                            ADD_STATUS_DUPLICATE;
                    }

                    int64_t minFee;
                    if (!canReplaceByFee(tx, oldTxIter->mTx, minFee))
                    {
                        tx->getResult().result.code(txINSUFFICIENT_FEE);
                        tx->getResult().feeCharged = minFee;
                        return TransactionQueue::AddResult::ADD_STATUS_ERROR;
                    }

                    oldTx = oldTxIter->mTx;
                    int64_t oldFee = oldTx->getFeeBid();
                    if (oldTx->getFeeSourceID() == tx->getFeeSourceID())
                    {
                        netFee -= oldFee;
                    }
                }

                if (oldTxIter != transactions.begin() &&
                    (tx->getMinSeqAge() != 0 || tx->getMinSeqLedgerGap() != 0))
                {
                    return TransactionQueue::AddResult::
                        ADD_STATUS_TRY_AGAIN_LATER;
                }

                // If this is a new tx, use the last seq in queue. If it's an
                // existing transaction, use the previous one in the queue (if
                // the tx is first, leave seqNum == 0 so the seqNum will be
                // loaded from the account)
                if (oldTxIter == transactions.end())
                {
                    seqNum = transactions.back().mTx->getSeqNum();
                }
                else if (oldTxIter != transactions.begin())
                {
                    auto copyIt = oldTxIter - 1;
                    seqNum = copyIt->mTx->getSeqNum();
                }
            }
        }
    }

    auto canAddRes = mTxQueueLimiter->canAddTx(tx, oldTx, txsToEvict);
    if (!canAddRes.first)
    {
        ban({tx});
        if (canAddRes.second != 0)
        {
            tx->getResult().result.code(txINSUFFICIENT_FEE);
            tx->getResult().feeCharged = canAddRes.second;
            return TransactionQueue::AddResult::ADD_STATUS_ERROR;
        }
        return TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER;
    }

    auto closeTime = mApp.getLedgerManager()
                         .getLastClosedLedgerHeader()
                         .header.scpValue.closeTime;

    // Transaction queue performs read-only transactions to the database and
    // there are no concurrent writers, so it is safe to not enclose all the SQL
    // statements into one transaction here.
    LedgerTxn ltx(mApp.getLedgerTxnRoot(), /* shouldUpdateLastModified */ true,
                  TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        // This is done so minSeqLedgerGap is validated against the next
        // ledgerSeq, which is what will be used at apply time
        ltx.loadHeader().current().ledgerSeq =
            mApp.getLedgerManager().getLastClosedLedgerNum() + 1;
    }
    if (!tx->checkValid(ltx, seqNum, 0,
                        getUpperBoundCloseTimeOffset(mApp, closeTime)))
    {
        return TransactionQueue::AddResult::ADD_STATUS_ERROR;
    }

    // Note: stateIter corresponds to getSourceID() which is not necessarily
    // the same as getFeeSourceID()
    auto feeSource = stellar::loadAccount(ltx, tx->getFeeSourceID());
    auto feeStateIter = mAccountStates.find(tx->getFeeSourceID());
    int64_t totalFees = feeStateIter == mAccountStates.end()
                            ? 0
                            : feeStateIter->second.mTotalFees;
    if (getAvailableBalance(ltx.loadHeader(), feeSource) - netFee < totalFees)
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return TransactionQueue::AddResult::ADD_STATUS_ERROR;
    }

    return TransactionQueue::AddResult::ADD_STATUS_PENDING;
}

void
TransactionQueue::releaseFeeMaybeEraseAccountState(TransactionFrameBasePtr tx)
{
    auto iter = mAccountStates.find(tx->getFeeSourceID());
    releaseAssert(iter != mAccountStates.end() &&
                  iter->second.mTotalFees >= tx->getFeeBid());

    iter->second.mTotalFees -= tx->getFeeBid();
    if (iter->second.mTransactions.empty())
    {
        if (iter->second.mTotalFees == 0)
        {
            mAccountStates.erase(iter);
        }
    }
}

void
TransactionQueue::prepareDropTransaction(AccountState& as, TimestampedTx& tstx)
{
    auto ops = tstx.mTx->getNumOperations();
    as.mQueueSizeOps -= ops;
    mTxQueueLimiter->removeTransaction(tstx.mTx);
    mKnownTxHashes.erase(tstx.mTx->getFullHash());
    CLOG_DEBUG(Tx, "Dropping {} transaction",
               hexAbbrev(tstx.mTx->getFullHash()));
    if (!tstx.mBroadcasted)
    {
        as.mBroadcastQueueOps -= ops;
    }
    releaseFeeMaybeEraseAccountState(tstx.mTx);
}

// Heuristic: an "arbitrage transaction" as identified by this function as any
// tx that has 1 or more path payments in it that collectively form a payment
// _loop_. That is: a tx that performs a sequence of order-book conversions of
// at least some quantity of some asset _back_ to itself via some number of
// intermediate steps. Typically these are only a single path-payment op, but
// for thoroughness sake we're also going to cover cases where there's any
// atomic _sequence_ of path payment ops that cause a conversion-loop.
//
// Such transactions are not going to be outright banned, note: just damped
// so that they do not overload the network. Currently people are submitting
// thousands of such txs per second in an attempt to win races for arbitrage,
// and we just want to make those races a behave more like bidding wars than
// pure resource-wasting races.
//
// This function doesn't catch all forms of arbitrage -- there are an unlimited
// number of types, many of which involve holding assets, interacting with
// real-world actors, etc. and are indistinguishable from "real" traffic -- but
// it does cover the case of zero-risk (fee-only) instantaneous-arbitrage
// attempts, which users are (at the time of writing) flooding the network with.
std::vector<AssetPair>
TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
    TransactionFrameBasePtr tx)
{
    std::map<Asset, size_t> assetToNum;
    std::vector<Asset> numToAsset;
    std::vector<BitSet> graph;

    auto internAsset = [&](Asset const& a) -> size_t {
        size_t n = numToAsset.size();
        auto pair = assetToNum.emplace(a, n);
        if (pair.second)
        {
            numToAsset.emplace_back(a);
            graph.emplace_back(BitSet());
        }
        return pair.first->second;
    };

    auto internEdge = [&](Asset const& src, Asset const& dst) {
        auto si = internAsset(src);
        auto di = internAsset(dst);
        graph.at(si).set(di);
    };

    auto internSegment = [&](Asset const& src, Asset const& dst,
                             std::vector<Asset> const& path) {
        Asset const* prev = &src;
        for (auto const& a : path)
        {
            internEdge(*prev, a);
            prev = &a;
        }
        internEdge(*prev, dst);
    };

    for (auto const& op : tx->getRawOperations())
    {
        switch (op.body.type())
        {
        case PATH_PAYMENT_STRICT_RECEIVE:
        {
            auto const& pop = op.body.pathPaymentStrictReceiveOp();
            internSegment(pop.sendAsset, pop.destAsset, pop.path);
        }
        break;
        case PATH_PAYMENT_STRICT_SEND:
        {
            auto const& pop = op.body.pathPaymentStrictSendOp();
            internSegment(pop.sendAsset, pop.destAsset, pop.path);
        }
        break;
        default:
            continue;
        }
    }

    // We build a TarjanSCCCalculator for the graph of all the edges we've seen,
    // and return the set of edges that participate in nontrivial SCCs (which
    // are loops). This is O(|v| + |e|) and just operations on a vector of pairs
    // of integers.

    TarjanSCCCalculator tsc;
    tsc.calculateSCCs(graph.size(), [&graph](size_t i) -> BitSet const& {
        // NB: this closure must be written with the explicit const&
        // returning type signature, otherwise it infers wrong and
        // winds up returning a dangling reference at its site of use.
        return graph.at(i);
    });

    std::vector<AssetPair> ret;
    for (BitSet const& scc : tsc.mSCCs)
    {
        if (scc.count() > 1)
        {
            for (size_t src = 0; scc.nextSet(src); ++src)
            {
                BitSet edgesFromSrcInSCC = graph.at(src);
                edgesFromSrcInSCC.inplaceIntersection(scc);
                for (size_t dst = 0; edgesFromSrcInSCC.nextSet(dst); ++dst)
                {
                    ret.emplace_back(
                        AssetPair{numToAsset.at(src), numToAsset.at(dst)});
                }
            }
        }
    }
    return ret;
}

TransactionQueue::AddResult
TransactionQueue::tryAdd(TransactionFrameBasePtr tx, bool submittedFromSelf)
{
    ZoneScoped;
    AccountStates::iterator stateIter;
    TimestampedTransactions::iterator oldTxIter;
    std::vector<std::pair<TxStackPtr, bool>> txsToEvict;
    auto const res = canAdd(tx, stateIter, oldTxIter, txsToEvict);
    if (res != TransactionQueue::AddResult::ADD_STATUS_PENDING)
    {
        return res;
    }

    if (stateIter == mAccountStates.end())
    {
        stateIter =
            mAccountStates.emplace(tx->getSourceID(), AccountState{}).first;
        oldTxIter = stateIter->second.mTransactions.end();
    }

    if (oldTxIter != stateIter->second.mTransactions.end())
    {
        prepareDropTransaction(stateIter->second, *oldTxIter);
        *oldTxIter = {tx, false, mApp.getClock().now(), submittedFromSelf};
    }
    else
    {
        stateIter->second.mTransactions.push_back(
            {tx, false, mApp.getClock().now(), submittedFromSelf});
        oldTxIter = --stateIter->second.mTransactions.end();
        mSizeByAge[stateIter->second.mAge]->inc();
    }
    auto ops = tx->getNumOperations();
    stateIter->second.mQueueSizeOps += ops;
    stateIter->second.mBroadcastQueueOps += ops;
    auto& thisAccountState = mAccountStates[tx->getFeeSourceID()];
    thisAccountState.mTotalFees += tx->getFeeBid();

    // make space so that we can add this transaction
    // this will succeed as `canAdd` ensures that this is the case
    mTxQueueLimiter->evictTransactions(
        txsToEvict, *tx,
        [&](TransactionFrameBasePtr const& txToEvict) { ban({txToEvict}); });
    mTxQueueLimiter->addTransaction(tx);
    mKnownTxHashes[tx->getFullHash()] = tx;

    broadcast(false);

    return res;
}

void
TransactionQueue::dropTransactions(AccountStates::iterator stateIter,
                                   TimestampedTransactions::iterator begin,
                                   TimestampedTransactions::iterator end)
{
    ZoneScoped;
    // Remove fees and update queue size for each transaction to be dropped.
    // Note prepareDropTransaction may erase other iterators from
    // mAccountStates, but it will not erase stateIter because it has at least
    // one transaction (otherwise we couldn't reach that line).
    for (auto iter = begin; iter != end; ++iter)
    {
        prepareDropTransaction(stateIter->second, *iter);
    }

    // Actually erase the transactions to be dropped.
    stateIter->second.mTransactions.erase(begin, end);

    // If the queue for stateIter is now empty, then (1) erase it if it is not
    // the fee-source for some other transaction or (2) reset the age otherwise.
    if (stateIter->second.mTransactions.empty())
    {
        if (stateIter->second.mTotalFees == 0)
        {
            mAccountStates.erase(stateIter);
        }
        else
        {
            stateIter->second.mAge = 0;
        }
    }
}

void
TransactionQueue::removeApplied(Transactions const& appliedTxs)
{
    ZoneScoped;
    // Find the highest sequence number that was applied for each source account
    std::map<AccountID, int64_t> seqByAccount;
    UnorderedSet<Hash> appliedHashes;
    appliedHashes.reserve(appliedTxs.size());
    for (auto const& tx : appliedTxs)
    {
        auto& seq = seqByAccount[tx->getSourceID()];
        seq = std::max(seq, tx->getSeqNum());
        appliedHashes.emplace(tx->getFullHash());
    }

    auto now = mApp.getClock().now();
    for (auto const& kv : seqByAccount)
    {
        // If the source account is not in mAccountStates, then it has no
        // transactions in the queue so there is nothing to do
        auto stateIter = mAccountStates.find(kv.first);
        if (stateIter != mAccountStates.end())
        {
            // If there are no transactions in the queue for this source
            // account, then there is nothing to do
            auto& transactions = stateIter->second.mTransactions;
            if (!transactions.empty())
            {
                // If the sequence number of the first transaction is greater
                // than the highest applied sequence number for this source
                // account, then there is nothing to do because sequence numbers
                // are monotonic (this shouldn't happen)
                if (transactions.front().mTx->getSeqNum() <= kv.second)
                {
                    // We care about matching the sequence number rather than
                    // the hash, because any transaction with a sequence number
                    // less-than-or-equal to the highest applied sequence number
                    // for this source account has either (1) been applied, or
                    // (2) become invalid.

                    // std::upper_bound returns an iterator to the first element
                    // in the range that is greater than kv.second, so we will
                    // erase up to that element in dropTransactions
                    auto txIter = std::upper_bound(
                        transactions.begin(), transactions.end(), kv.second,
                        [](int64_t a,
                           TransactionQueue::TimestampedTx const& b) {
                            return a < b.mTx->getSeqNum();
                        });

                    // The age is going to be reset because at least one
                    // transaction was applied for this account. This means that
                    // the size for the current age will decrease by the total
                    // number of transactions in the queue, while the size for
                    // the new age (0) will only include the transactions that
                    // were not removed
                    auto& age = stateIter->second.mAge;
                    mSizeByAge[age]->dec(transactions.size());
                    age = 0;
                    mSizeByAge[0]->inc(transactions.end() - txIter);

                    // update the metric for the time spent for applied
                    // transactions using exact match
                    for (auto it = transactions.begin(); it != txIter; ++it)
                    {
                        if (appliedHashes.find(it->mTx->getFullHash()) !=
                            appliedHashes.end())
                        {
                            auto elapsed = now - it->mInsertionTime;
                            mTransactionsDelay.Update(elapsed);
                            if (it->mSubmittedFromSelf)
                            {
                                mTransactionsSelfDelay.Update(elapsed);
                            }
                        }
                    }

                    // WARNING: stateIter and everything that references it may
                    // be invalid from this point onward and should not be used.
                    dropTransactions(stateIter, transactions.begin(), txIter);
                }
            }
        }
    }

    for (auto const& h : appliedHashes)
    {
        auto& bannedFront = mBannedTransactions.front();
        bannedFront.emplace(h);
        CLOG_DEBUG(Tx, "Ban applied transaction {}", hexAbbrev(h));

        // do not mark metric for banning as this is the result of normal flow
        // of operations
    }
}

static void
findTx(TransactionFrameBasePtr tx,
       TransactionQueue::TimestampedTransactions& transactions,
       TransactionQueue::TimestampedTransactions::iterator& txIter)
{
    auto iter = transactions.end();
    findBySeq(tx, transactions, iter);
    if (iter != transactions.end() &&
        iter->mTx->getFullHash() == tx->getFullHash())
    {
        txIter = iter;
    }
}

void
TransactionQueue::ban(Transactions const& banTxs)
{
    ZoneScoped;
    auto& bannedFront = mBannedTransactions.front();

    // Group the transactions by source account and ban all the transactions
    // that are explicitly listed
    std::map<AccountID, Transactions> transactionsByAccount;
    for (auto const& tx : banTxs)
    {
        auto& transactions = transactionsByAccount[tx->getSourceID()];
        transactions.emplace_back(tx);
        CLOG_DEBUG(Tx, "Ban transaction {}", hexAbbrev(tx->getFullHash()));
        if (bannedFront.emplace(tx->getFullHash()).second)
        {
            mBannedTransactionsCounter.inc();
        }
    }

    for (auto const& kv : transactionsByAccount)
    {
        // If the source account is not in mAccountStates, then it has no
        // transactions in the queue so there is nothing to do
        auto stateIter = mAccountStates.find(kv.first);
        if (stateIter != mAccountStates.end())
        {
            // If there are no transactions in the queue for this source
            // account, then there is nothing to do
            auto& transactions = stateIter->second.mTransactions;
            if (!transactions.empty())
            {
                // We need to find the banned transaction by hash with the
                // lowest sequence number; this will be represented by txIter.
                // If txIter is past-the-end then we will not remove any
                // transactions. Note that the explicitly banned transactions
                // for this source account are not sorted.
                auto txIter = transactions.end();
                for (auto const& tx : kv.second)
                {
                    if (txIter == transactions.end() ||
                        tx->getSeqNum() < txIter->mTx->getSeqNum())
                    {
                        // findTx does nothing unless tx matches-by-hash with
                        // a transaction in transactions.
                        findTx(tx, transactions, txIter);
                    }
                }

                // Ban all the transactions that follow the first matching
                // banned transaction, because they no longer have the right
                // sequence number to be in the queue. Also adjust the size
                // for this age.
                for (auto iter = txIter; iter != transactions.end(); ++iter)
                {
                    if (bannedFront.emplace(iter->mTx->getFullHash()).second)
                    {
                        mBannedTransactionsCounter.inc();
                    }
                }
                mSizeByAge[stateIter->second.mAge]->dec(transactions.end() -
                                                        txIter);

                // Drop all of the transactions, release fees (which can
                // cause other accounts to be removed from mAccountStates),
                // and potentially remove this account from mAccountStates.
                // WARNING: stateIter and everything that references it may
                // be invalid from this point onward and should not be used.
                dropTransactions(stateIter, txIter, transactions.end());
            }
        }
    }
}

TransactionQueue::AccountTxQueueInfo
TransactionQueue::getAccountTransactionQueueInfo(
    AccountID const& accountID) const
{
    auto i = mAccountStates.find(accountID);
    if (i == std::end(mAccountStates))
    {
        return {0, 0, 0, 0, 0};
    }

    auto& as = i->second;
    auto const& txs = as.mTransactions;
    auto seqNum = txs.empty() ? 0 : txs.back().mTx->getSeqNum();
    return {seqNum, as.mTotalFees, as.mQueueSizeOps, as.mBroadcastQueueOps,
            as.mAge};
}

void
TransactionQueue::shift()
{
    ZoneScoped;
    mBannedTransactions.pop_back();
    mBannedTransactions.emplace_front();
    mArbitrageFloodDamping.clear();

    auto sizes = std::vector<int64_t>{};
    sizes.resize(mPendingDepth);

    auto& bannedFront = mBannedTransactions.front();
    auto end = std::end(mAccountStates);
    auto it = std::begin(mAccountStates);
    while (it != end)
    {
        // If mTransactions is empty then mAge is always 0. This can occur if an
        // account is the fee-source for at least one transaction but not the
        // sequence-number-source for any transaction in the TransactionQueue.
        if (!it->second.mTransactions.empty())
        {
            ++it->second.mAge;
        }

        if (mPendingDepth == it->second.mAge)
        {
            for (auto& toBan : it->second.mTransactions)
            {
                // This never invalidates it because
                //     !it->second.mTransactions.empty()
                // otherwise we couldn't have reached this line.
                prepareDropTransaction(it->second, toBan);
                CLOG_DEBUG(Tx, "Ban transaction {}",
                           hexAbbrev(toBan.mTx->getFullHash()));
                bannedFront.insert(toBan.mTx->getFullHash());
            }
            mBannedTransactionsCounter.inc(
                static_cast<int64_t>(it->second.mTransactions.size()));
            it->second.mTransactions.clear();
            if (it->second.mTotalFees == 0)
            {
                it = mAccountStates.erase(it);
            }
            else
            {
                it->second.mAge = 0;
            }
        }
        else
        {
            sizes[it->second.mAge] +=
                static_cast<int64_t>(it->second.mTransactions.size());
            ++it;
        }
    }

    for (size_t i = 0; i < sizes.size(); i++)
    {
        mSizeByAge[i]->set_count(sizes[i]);
    }
    mTxQueueLimiter->resetEvictionState();
    // pick a new randomizing seed for tie breaking
    mBroadcastSeed =
        rand_uniform<uint64>(0, std::numeric_limits<uint64>::max());
}

size_t
TransactionQueue::countBanned(int index) const
{
    return mBannedTransactions[index].size();
}

bool
TransactionQueue::isBanned(Hash const& hash) const
{
    return std::any_of(
        std::begin(mBannedTransactions), std::end(mBannedTransactions),
        [&](UnorderedSet<Hash> const& transactions) {
            return transactions.find(hash) != std::end(transactions);
        });
}

TxSetFrame::Transactions
TransactionQueue::getTransactions(LedgerHeader const& lcl) const
{
    ZoneScoped;
    TxSetFrame::Transactions txs;

    uint32_t const nextLedgerSeq = lcl.ledgerSeq + 1;
    int64_t const startingSeq = getStartingSequenceNumber(nextLedgerSeq);
    for (auto const& m : mAccountStates)
    {
        for (auto const& tx : m.second.mTransactions)
        {
            // This guarantees that a node will never nominate a transaction set
            // containing a transaction with seqNum == startingSeq. This is
            // required to support the analogous transaction validity condition
            // in TransactionFrame::isBadSeq. As a consequence, all transactions
            // for a source account will either have
            //     - sequence numbers above startingSeq, or
            //     - sequence numbers below startingSeq.
            if (tx.mTx->getSeqNum() == startingSeq)
            {
                break;
            }
            txs.emplace_back(tx.mTx);
        }
    }

    return txs;
}

TransactionFrameBaseConstPtr
TransactionQueue::getTx(Hash const& hash) const
{
    ZoneScoped;
    auto it = mKnownTxHashes.find(hash);
    if (it != mKnownTxHashes.end())
    {
        return it->second;
    }
    else
    {
        return nullptr;
    }
}

void
TransactionQueue::clearAll()
{
    mAccountStates.clear();
    for (auto& b : mBannedTransactions)
    {
        b.clear();
    }
    mTxQueueLimiter->reset();
    mKnownTxHashes.clear();
}

void
TransactionQueue::maybeVersionUpgraded()
{
    auto const& lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (protocolVersionIsBefore(mLedgerVersion, ProtocolVersion::V_13) &&
        protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  ProtocolVersion::V_13))
    {
        clearAll();
    }
    mLedgerVersion = lcl.header.ledgerVersion;
}

std::pair<uint32_t, std::optional<uint32_t>>
TransactionQueue::getMaxOpsToFloodThisPeriod() const
{
    auto& cfg = mApp.getConfig();
    double opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;

    auto maxOps = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    double opsToFloodLedgerDbl = opRatePerLedger * maxOps;
    releaseAssertOrThrow(opsToFloodLedgerDbl >= 0.0);
    releaseAssertOrThrow(isRepresentableAsInt64(opsToFloodLedgerDbl));
    int64_t opsToFloodLedger = static_cast<int64_t>(opsToFloodLedgerDbl);

    int64_t opsToFlood =
        mBroadcastOpCarryover[SurgePricingPriorityQueue::GENERIC_LANE] +
        bigDivideOrThrow(opsToFloodLedger, cfg.FLOOD_TX_PERIOD_MS,
                         cfg.getExpectedLedgerCloseTime().count() * 1000,
                         Rounding::ROUND_UP);
    releaseAssertOrThrow(opsToFlood >= 0 &&
                         opsToFlood <= std::numeric_limits<uint32_t>::max());

    auto maxDexOps = cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET;
    std::optional<uint32_t> dexOpsToFlood;
    if (maxDexOps)
    {
        *maxDexOps = std::min(maxOps, *maxDexOps);
        uint32_t dexOpsToFloodLedger =
            static_cast<uint32_t>(*maxDexOps * opRatePerLedger);
        uint32_t dexOpsCarryover =
            mBroadcastOpCarryover.size() > DexLimitingLaneConfig::DEX_LANE
                ? mBroadcastOpCarryover[DexLimitingLaneConfig::DEX_LANE]
                : 0;
        dexOpsToFlood =
            dexOpsCarryover +
            bigDivideOrThrow(dexOpsToFloodLedger, cfg.FLOOD_TX_PERIOD_MS,
                             cfg.getExpectedLedgerCloseTime().count() * 1000,
                             Rounding::ROUND_UP);
    }
    return std::make_pair(static_cast<uint32_t>(opsToFlood), dexOpsToFlood);
}

TransactionQueue::BroadcastStatus
TransactionQueue::broadcastTx(AccountState& state, TimestampedTx& tx)
{
    if (tx.mBroadcasted)
    {
        return BroadcastStatus::BROADCAST_STATUS_ALREADY;
    }

    bool allowTx{true};
    int32_t const signedAllowance =
        mApp.getConfig().FLOOD_ARB_TX_BASE_ALLOWANCE;
    if (signedAllowance >= 0)
    {
        uint32_t const allowance = static_cast<uint32_t>(signedAllowance);

        // If arb tx damping is enabled, we only flood the first few arb txs
        // touching an asset pair in any given ledger, exponentially reducing
        // the odds of further arb tx broadcast on a per-asset-pair basis. This
        // lets _some_ arbitrage occur (and retains price-based competition
        // among arbitrageurs earlier in the queue) but avoids filling up
        // ledgers with excessive (mostly failed) arb attempts.
        auto arbPairs = findAllAssetPairsInvolvedInPaymentLoops(tx.mTx);
        if (!arbPairs.empty())
        {
            mArbTxSeenCounter.inc();
            uint32_t maxBroadcast{0};
            std::vector<
                UnorderedMap<AssetPair, uint32_t, AssetPairHash>::iterator>
                hashMapIters;

            // NB: it's essential to reserve() on the hashmap so that we
            // can store iterators to positions in it _as we emplace them_
            // in the loop that follows, without rehashing. Do not remove.
            mArbitrageFloodDamping.reserve(mArbitrageFloodDamping.size() +
                                           arbPairs.size());

            for (auto const& key : arbPairs)
            {
                auto pair = mArbitrageFloodDamping.emplace(key, 0);
                hashMapIters.emplace_back(pair.first);
                maxBroadcast = std::max(maxBroadcast, pair.first->second);
            }

            // Admit while no pair on the path has hit the allowance.
            allowTx = maxBroadcast < allowance;

            // If any pair is over the allowance, dampen transmission randomly
            // based on it.
            if (!allowTx)
            {
                std::geometric_distribution<uint32_t> dist(
                    mApp.getConfig().FLOOD_ARB_TX_DAMPING_FACTOR);
                uint32_t k = maxBroadcast - allowance;
                allowTx = dist(gRandomEngine) >= k;
            }

            // If we've decided to admit a tx, bump all pairs on the path.
            if (allowTx)
            {
                for (auto i : hashMapIters)
                {
                    i->second++;
                }
            }
            else
            {
                mArbTxDroppedCounter.inc();
            }
        }
    }
#ifdef BUILD_TESTS
    if (mTxBroadcastedEvent)
    {
        mTxBroadcastedEvent(tx.mTx);
    }
#endif

    // Mark the tx as effectively "broadcast" and update the per-account queue
    // to count it as consumption from that balance, for proper overall queue
    // accounting (whether or not we will actually broadcast it).
    tx.mBroadcasted = true;
    state.mBroadcastQueueOps -= tx.mTx->getNumOperations();

    if (!allowTx)
    {
        // If we decide not to broadcast for real (due to damping) we return
        // false to our caller so that they will not count this tx against the
        // per-timeslice counters -- we want to allow the caller to try useful
        // work from other sources.
        return BroadcastStatus::BROADCAST_STATUS_SKIPPED;
    }
    return mApp.getOverlayManager().broadcastMessage(
               tx.mTx->toStellarMessage(), false,
               std::make_optional<Hash>(tx.mTx->getFullHash()))
               ? BroadcastStatus::BROADCAST_STATUS_SUCCESS
               : BroadcastStatus::BROADCAST_STATUS_ALREADY;
}

class TxQueueTracker : public TxStack
{
  public:
    TxQueueTracker(TransactionQueue::AccountState* accountState)
        : mAccountState(accountState)
    {
        mCur = mAccountState->mTransactions.begin();
        skipToFirstNotBroadcasted();
        // there should be at least one tx to broadcast in this queue
        releaseAssert(!empty());
    }

    TransactionFrameBasePtr
    getTopTx() const override
    {
        releaseAssert(!empty());
        return mCur->mTx;
    }

    TransactionQueue::TimestampedTx&
    getCurrentTimestampedTx() const
    {
        return *mCur;
    }

    bool
    empty() const override
    {
        return mCur == mAccountState->mTransactions.end();
    }

    void
    popTopTx() override
    {
        ++mCur;
        skipToFirstNotBroadcasted();
    }

    uint32_t
    getNumOperations() const override
    {
        // Operation count tracking is not relevant for this TxStack.
        return 0;
    }

    TransactionQueue::AccountState&
    getAccountState() const
    {
        return *mAccountState;
    }

  private:
    // skips to first transaction not broadcasted yet
    void
    skipToFirstNotBroadcasted()
    {
        while (mCur != mAccountState->mTransactions.end() && mCur->mBroadcasted)
        {
            ++mCur;
        }
    }

    TransactionQueue::TimestampedTransactions::iterator mCur;
    TransactionQueue::AccountState* mAccountState;
};

bool
TransactionQueue::broadcastSome()
{
    // broadcast transactions in surge pricing order:
    // loop over transactions by picking from the account queue with the
    // highest base fee not broadcasted so far.
    // This broadcasts from account queues in order as to maximize chances of
    // propagation.
    auto [opsToFlood, dexOpsToFlood] = getMaxOpsToFloodThisPeriod();

    size_t totalOpsToFlood = 0;
    std::vector<TxStackPtr> trackersToBroadcast;
    for (auto& [_, accountState] : mAccountStates)
    {
        auto asOps = accountState.mBroadcastQueueOps;
        if (asOps != 0)
        {
            trackersToBroadcast.emplace_back(
                std::make_shared<TxQueueTracker>(&accountState));
            totalOpsToFlood += asOps;
        }
    }

    std::vector<TransactionFrameBasePtr> banningTxs;
    auto visitor = [this, &totalOpsToFlood,
                    &banningTxs](TxStack const& txStack) {
        auto const& curTracker = static_cast<TxQueueTracker const&>(txStack);
        // look at the next candidate transaction for that account
        auto& cur = curTracker.getCurrentTimestampedTx();
        auto tx = curTracker.getTopTx();
        // by construction, cur points to non broadcasted transactions
        releaseAssert(!cur.mBroadcasted);
        auto bStatus = broadcastTx(curTracker.getAccountState(), cur);
        if (bStatus == BroadcastStatus::BROADCAST_STATUS_SUCCESS)
        {
            totalOpsToFlood -= tx->getNumOperations();
            return SurgePricingPriorityQueue::VisitTxStackResult::TX_PROCESSED;
        }
        else if (bStatus == BroadcastStatus::BROADCAST_STATUS_SKIPPED)
        {
            // When skipping, we ban the transaction and skip the remainder of
            // the stack.
            banningTxs.emplace_back(tx);
            return SurgePricingPriorityQueue::VisitTxStackResult::
                TX_STACK_SKIPPED;
        }
        else
        {
            // Already broadcasted; don't invalidate the stack but also don't
            // count transaction as processed.
            return SurgePricingPriorityQueue::VisitTxStackResult::TX_SKIPPED;
        }
    };
    SurgePricingPriorityQueue::visitTopTxs(
        trackersToBroadcast,
        std::make_shared<DexLimitingLaneConfig>(opsToFlood, dexOpsToFlood),
        mBroadcastSeed, visitor, mBroadcastOpCarryover);
    ban(banningTxs);
    // carry over remainder, up to MAX_OPS_PER_TX ops
    // reason is that if we add 1 next round, we can flood a "worst case fee
    // bump" tx
    for (auto& opsLeft : mBroadcastOpCarryover)
    {
        opsLeft = std::min(opsLeft, MAX_OPS_PER_TX + 1);
    }
    return totalOpsToFlood != 0;
}

void
TransactionQueue::broadcast(bool fromCallback)
{
    if (mShutdown || (!fromCallback && mWaiting))
    {
        return;
    }
    mWaiting = false;

    bool needsMore = false;
    if (!fromCallback)
    {
        // don't do anything right away, wait for the timer
        needsMore = true;
    }
    else
    {
        needsMore = broadcastSome();
    }

    if (needsMore)
    {
        mWaiting = true;
        mBroadcastTimer.expires_from_now(
            std::chrono::milliseconds(mApp.getConfig().FLOOD_TX_PERIOD_MS));
        mBroadcastTimer.async_wait([&]() { broadcast(true); },
                                   &VirtualTimer::onFailureNoop);
    }
}

void
TransactionQueue::rebroadcast()
{
    // force to rebroadcast everything
    for (auto& m : mAccountStates)
    {
        auto& as = m.second;
        as.mBroadcastQueueOps = as.mQueueSizeOps;
        for (auto& tx : as.mTransactions)
        {
            tx.mBroadcasted = false;
        }
    }
    broadcast(false);
}

void
TransactionQueue::shutdown()
{
    mShutdown = true;
    mBroadcastTimer.cancel();
}

bool
operator==(TransactionQueue::AccountTxQueueInfo const& x,
           TransactionQueue::AccountTxQueueInfo const& y)
{
    return x.mMaxSeq == y.mMaxSeq && x.mTotalFees == y.mTotalFees &&
           x.mQueueSizeOps == y.mQueueSizeOps &&
           x.mBroadcastQueueOps == y.mBroadcastQueueOps;
}

static bool
containsFilteredOperation(std::vector<Operation> const& ops,
                          UnorderedSet<OperationType> const& filteredTypes)
{
    return std::any_of(ops.begin(), ops.end(), [&](auto const& op) {
        return filteredTypes.find(op.body.type()) != filteredTypes.end();
    });
}

bool
TransactionQueue::isFiltered(TransactionFrameBasePtr tx) const
{
    // Avoid cost of checking if filtering is not in use
    if (mFilteredTypes.empty())
    {
        return false;
    }

    switch (tx->getEnvelope().type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return containsFilteredOperation(tx->getEnvelope().v0().tx.operations,
                                         mFilteredTypes);
    case ENVELOPE_TYPE_TX:
        return containsFilteredOperation(tx->getEnvelope().v1().tx.operations,
                                         mFilteredTypes);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
    {
        auto const& envelope = tx->getEnvelope().feeBump().tx.innerTx.v1();
        return containsFilteredOperation(envelope.tx.operations,
                                         mFilteredTypes);
    }
    default:
        abort();
    }
}

#ifdef BUILD_TESTS
size_t
TransactionQueue::getQueueSizeOps() const
{
    return mTxQueueLimiter->size();
}

std::optional<int64_t>
TransactionQueue::getInQueueSeqNum(AccountID const& account) const
{
    auto stateIter = mAccountStates.find(account);
    if (stateIter == mAccountStates.end())
    {
        return std::nullopt;
    }
    auto& transactions = stateIter->second.mTransactions;
    return transactions.back().mTx->getSeqNum();
}
#endif

size_t
TransactionQueue::getMaxQueueSizeOps() const
{
    return mTxQueueLimiter->maxQueueSizeOps();
}

}
