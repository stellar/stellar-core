// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TransactionQueue.h"
#include "crypto/SecretKey.h"
#include "herder/TxQueueLimiter.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/HashOfHash.h"
#include "util/XDROperators.h"

#include <Tracy.hpp>
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <numeric>

namespace stellar
{
const int64_t TransactionQueue::FEE_MULTIPLIER = 10;

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
    , mTransactionsDelay(
          app.getMetrics().NewTimer({"herder", "pending-txs", "delay"}))
    , mBroadcastTimer(app)
{
    mTxQueueLimiter = std::make_unique<TxQueueLimiter>(poolLedgerMultiplier,
                                                       app.getLedgerManager());
    for (uint32 i = 0; i < pendingDepth; i++)
    {
        mSizeByAge.emplace_back(&app.getMetrics().NewCounter(
            {"herder", "pending-txs", fmt::format("age{}", i)}));
    }

    auto const& filteredTypes =
        app.getConfig().EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE;
    mFilteredTypes.insert(filteredTypes.begin(), filteredTypes.end());
    mBroadcastSeed =
        rand_uniform<uint64>(0, std::numeric_limits<uint64>::max());
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
    auto v1 = bigMultiply(newFee, oldNumOps);
    auto v2 = bigMultiply(oldFee, newNumOps);
    auto minFeeN = v2 * TransactionQueue::FEE_MULTIPLIER;
    bool res = v1 >= minFeeN;
    if (!res)
    {
        if (!bigDivide(minFee, minFeeN, int64_t(oldNumOps), Rounding::ROUND_UP))
        {
            minFee = INT64_MAX;
        }
    }
    return res;
}

static bool
findBySeq(int64_t seq, TransactionQueue::TimestampedTransactions& transactions,
          TransactionQueue::TimestampedTransactions::iterator& iter)
{
    int64_t firstSeq = transactions.front().mTx->getSeqNum();
    int64_t lastSeq = transactions.back().mTx->getSeqNum();
    if (seq < firstSeq || seq > lastSeq + 1)
    {
        return false;
    }

    assert(seq - firstSeq <= static_cast<int64_t>(transactions.size()));
    iter = transactions.begin() + (seq - firstSeq);
    assert(iter == transactions.end() || iter->mTx->getSeqNum() == seq);
    return true;
}

static bool
findBySeq(TransactionFrameBasePtr tx,
          TransactionQueue::TimestampedTransactions& transactions,
          TransactionQueue::TimestampedTransactions::iterator& iter)
{
    return findBySeq(tx->getSeqNum(), transactions, iter);
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
                         TimestampedTransactions::iterator& oldTxIter)
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

                seqNum = tx->getSeqNum() - 1;
            }
        }
    }

    auto canAddRes = mTxQueueLimiter->canAddTx(tx, oldTx);
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
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
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
    assert(iter != mAccountStates.end() &&
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
    if (!tstx.mBroadcasted)
    {
        as.mBroadcastQueueOps -= ops;
    }
    releaseFeeMaybeEraseAccountState(tstx.mTx);
}

TransactionQueue::AddResult
TransactionQueue::tryAdd(TransactionFrameBasePtr tx)
{
    ZoneScoped;
    AccountStates::iterator stateIter;
    TimestampedTransactions::iterator oldTxIter;
    auto const res = canAdd(tx, stateIter, oldTxIter);
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
        *oldTxIter = {tx, false, mApp.getClock().now()};
    }
    else
    {
        stateIter->second.mTransactions.push_back(
            {tx, false, mApp.getClock().now()});
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
    if (!mTxQueueLimiter->evictTransactions(
            ops, [&](TransactionFrameBasePtr const& txToEvict) {
                ban({txToEvict});
            }))
    {
        throw std::logic_error(
            "Invalid queue state, could not evict transactions");
    }
    mTxQueueLimiter->addTransaction(tx);

    if (mApp.getConfig().FLOOD_TX_PERIOD_MS != 0)
    {
        broadcast(false);
    }
    else
    {
        broadcastTx(thisAccountState, *oldTxIter);
    }

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
                    auto txIter = transactions.end();
                    findBySeq(kv.second, transactions, txIter);

                    // Iterators define half-open ranges, but we need to include
                    // the transaction with the highest applied sequence number
                    if (txIter != transactions.end())
                    {
                        ++txIter;
                    }

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
                        }
                    }

                    // WARNING: stateIter and everything that references it may
                    // be invalid from this point onward and should not be used.
                    dropTransactions(stateIter, transactions.begin(), txIter);
                }
            }
        }
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

    for (auto i = 0; i < sizes.size(); i++)
    {
        mSizeByAge[i]->set_count(sizes[i]);
    }
    mTxQueueLimiter->resetMinFeeNeeded();
    // pick a new randomizing seed for tie breaking
    mBroadcastSeed =
        rand_uniform<uint64>(0, std::numeric_limits<uint64>::max());
}

int
TransactionQueue::countBanned(int index) const
{
    return static_cast<int>(mBannedTransactions[index].size());
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

std::shared_ptr<TxSetFrame>
TransactionQueue::toTxSet(LedgerHeaderHistoryEntry const& lcl) const
{
    ZoneScoped;
    auto result = std::make_shared<TxSetFrame>(lcl.hash);

    uint32_t const nextLedgerSeq = lcl.header.ledgerSeq + 1;
    int64_t const startingSeq = getStartingSequenceNumber(nextLedgerSeq);
    for (auto const& m : mAccountStates)
    {
        for (auto const& tx : m.second.mTransactions)
        {
            // The previous version of this enforced the following constraint:
            // there may be any number of transactions for a given source
            // account, but all transactions for that source account must
            // satisfy one of the following mutually exclusive conditions
            // (1) sequence number <= startingSeq - 1
            // (2) sequence number >= startingSeq
            //
            // This version enforces the following constraint: it is forbidden
            // to include a transaction with
            //     sequence number == startingSeq
            // The new condition is strictly stronger. First, note that the
            // sequence numbers (assuming 0 < k < n, and the source account has
            // initial number startingSeq - n - 1)
            //     startingSeq - n, ..., startingSeq - n + k
            // would be accepted by the new condition and by condition (1)
            // above. Second, note that the sequence numbers (assuming 0 < k,
            // 0 < n, and the source account has initial sequence number
            // startingSeq + n - 1)
            //     startingSeq + n, ..., startingSeq + n + k
            // would be accepted by the new condition and by condition (2)
            // above. These are the only sequence numbers that would be accepted
            // by the new condition. But the old condition would also accept
            // (assuming 0 < k, and the source account has initial sequence
            // number startingSeq - 1)
            //     startingSeq, ..., startingSeq + k .
            if (tx.mTx->getSeqNum() == startingSeq)
            {
                break;
            }
            result->add(tx.mTx);
        }
    }

    return result;
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
}

void
TransactionQueue::maybeVersionUpgraded()
{
    std::vector<ReplacedTransaction> res;

    auto const& lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (mLedgerVersion < 13 && lcl.header.ledgerVersion >= 13)
    {
        clearAll();
    }
    mLedgerVersion = lcl.header.ledgerVersion;
}

size_t
TransactionQueue::getMaxOpsToFloodThisPeriod() const
{
    size_t opsToFloodLedger;
    auto& cfg = mApp.getConfig();
    auto const opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;
    size_t maxOps = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    opsToFloodLedger = static_cast<size_t>(opRatePerLedger * maxOps);

    size_t opsToFlood;
    if (mApp.getConfig().FLOOD_TX_PERIOD_MS != 0)
    {
        opsToFlood = mBroadcastOpCarryover +
                     bigDivide(opsToFloodLedger, cfg.FLOOD_TX_PERIOD_MS,
                               cfg.getExpectedLedgerCloseTime().count() * 1000,
                               Rounding::ROUND_UP);
    }
    else
    {
        // else, flood the target at once
        opsToFlood = opsToFloodLedger;
    }
    return opsToFlood;
}

bool
TransactionQueue::broadcastTx(AccountState& state, TimestampedTx& tx)
{
    if (tx.mBroadcasted)
    {
        return false;
    }
#ifdef BUILD_TESTS
    if (mTxBroadcastedEvent)
    {
        mTxBroadcastedEvent(tx.mTx);
    }
#endif
    tx.mBroadcasted = true;
    state.mBroadcastQueueOps -= tx.mTx->getNumOperations();
    return mApp.getOverlayManager().broadcastMessage(
        tx.mTx->toStellarMessage());
}

struct TxQueueTracker
{
    TransactionQueue::TimestampedTransactions::iterator mCur;
    TransactionQueue::AccountState* mAccountState;

    // skips to first transaction not broadcasted yet
    bool
    skipToFirstNotBroadcasted()
    {
        while (mCur != mAccountState->mTransactions.end() && mCur->mBroadcasted)
        {
            ++mCur;
        }
        return mCur != mAccountState->mTransactions.end();
    }

    struct Comparator
    {
        size_t mSeed;
        Comparator(size_t seed) : mSeed(seed)
        {
        }

        // return true if l < r
        // compares tx `cur` for each TxTracker as to implement surge
        // pricing comparison
        bool
        operator()(TxQueueTracker const& l, TxQueueTracker const& r) const
        {
            if (l.mCur == l.mAccountState->mTransactions.end())
            {
                return r.mCur != r.mAccountState->mTransactions.end();
            }
            if (r.mCur == r.mAccountState->mTransactions.end())
            {
                return false;
            }
            return lessThanXored(l.mCur->mTx, r.mCur->mTx, mSeed);
        }
    };
};

bool
TransactionQueue::broadcastSome()
{
    // broadcast transactions in surge pricing order:
    // loop over transactions by picking from the account queue with the
    // highest base fee not broadcasted so far.
    // This broadcasts from account queues in order as to maximize chances of
    // propagation.
    size_t opsToFlood = getMaxOpsToFloodThisPeriod();

    // uses a priority queue of trackers, using a custom comparator as to
    // prioritize higher fee queues
    TxQueueTracker::Comparator comp(mBroadcastSeed);
    std::priority_queue<TxQueueTracker, std::vector<TxQueueTracker>,
                        TxQueueTracker::Comparator>
        iters(comp);
    size_t totalOpsToFlood = 0;
    for (auto& m : mAccountStates)
    {
        auto asOps = m.second.mBroadcastQueueOps;
        if (asOps != 0)
        {
            TxQueueTracker tracker{m.second.mTransactions.begin(), &m.second};
            auto r = tracker.skipToFirstNotBroadcasted();
            // there should be at least one tx to broadcast in this queue
            releaseAssert(r);
            iters.emplace(tracker);
            totalOpsToFlood += asOps;
        }
    }

    while (opsToFlood != 0 && !iters.empty())
    {
        auto curTracker = iters.top();
        iters.pop();
        // look at the next candidate transaction for that account
        auto& cur = curTracker.mCur;
        auto& tx = cur->mTx;
        // by construction, cur points to non broadcasted transactions
        releaseAssert(!cur->mBroadcasted);
        if (opsToFlood < tx->getNumOperations())
        {
            // as we can't flood this transaction we have to wait to
            // accumulate more flooding credit
            break;
        }
        else
        {
            if (broadcastTx(*curTracker.mAccountState, *cur))
            {
                auto ops = tx->getNumOperations();
                opsToFlood -= ops;
                totalOpsToFlood -= ops;
            }
            cur++;
        }
        // if we're not done with this account, add the tracker back
        if (curTracker.skipToFirstNotBroadcasted())
        {
            iters.push(curTracker);
        }
    }
    // carry over remainder, up to MAX_OPS_PER_TX ops
    // reason is that if we add 1 next round, we can flood a "worst case fee
    // bump" tx
    mBroadcastOpCarryover = std::min<size_t>(opsToFlood, MAX_OPS_PER_TX + 1);
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
    if (mApp.getConfig().FLOOD_TX_PERIOD_MS != 0 && !fromCallback)
    {
        // don't do anything right away, wait for the timer
        needsMore = true;
    }
    else
    {
        needsMore = broadcastSome();
    }

    if (mApp.getConfig().FLOOD_TX_PERIOD_MS != 0 && needsMore)
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
#endif
}
