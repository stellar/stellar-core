// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TransactionQueue.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"

#include <lib/util/format.h>
#include <medida/counter.h>
#include <medida/metrics_registry.h>
#include <numeric>

namespace stellar
{

TransactionQueue::TransactionQueue(Application& app, size_t pendingDepth,
                                   size_t banDepth)
    : mApp(app), mPendingDepth(pendingDepth), mBannedTransactions(banDepth)
{
    for (auto i = 0; i < pendingDepth; i++)
    {
        mSizeByAge.emplace_back(&app.getMetrics().NewCounter(
            {"herder", "pending-txs", fmt::format("age{}", i)}));
    }
}

void
TransactionQueue::addFee(TransactionFrameBasePtr const& tx)
{
    auto& fee = mTotalFees[tx->getFeeSourceID()];
    if (INT64_MAX - fee < tx->getFeeBid())
    {
        throw std::runtime_error("Fee overflow");
    }
    fee += tx->getFeeBid();
}

void
TransactionQueue::removeFee(TransactionFrameBasePtr const& tx)
{
    auto& fee = mTotalFees[tx->getFeeSourceID()];
    if (fee < tx->getFeeBid())
    {
        throw std::runtime_error("Total fees negative");
    }
    fee -= tx->getFeeBid();

    if (fee == 0)
    {
        mTotalFees.erase(tx->getFeeSourceID());
    }
}

TransactionQueue::AccountTransactions::Transactions::iterator
TransactionQueue::findTx(AccountTransactions::Transactions& transactions,
                         TransactionFrameBasePtr const& tx)
{
    if (transactions.empty())
    {
        return transactions.end();
    }

    // Note: start > 0 because every transaction in the TransactionQueue is
    //       valid, and valid transactions have positive sequence numbers.
    //       seqNum is unconstrained user input.
    int64_t seqNum = tx->getSeqNum();
    int64_t start = transactions.front().front()->getSeqNum();
    if (seqNum >= start && (seqNum - start) < transactions.size())
    {
        auto iter = transactions.begin() + (seqNum - start);
        if (iter->empty() || iter->front()->getSeqNum() != seqNum)
        {
            throw std::runtime_error("unexpected transaction queue state");
        }
        if (iter->front()->getInnerHash() == tx->getInnerHash())
        {
            return iter;
        }
    }
    return transactions.end();
}

int64_t
TransactionQueue::getTotalFee(AccountID const& feeSource) const
{
    auto feeIter = mTotalFees.find(feeSource);
    return (feeIter == mTotalFees.end()) ? 0 : feeIter->second;
}

TransactionQueue::AddResult
TransactionQueue::validateAndMaybeAddTransaction(
    TransactionFrameBasePtr const& tx, int64_t validationSeqNum)
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    if (!tx->checkValid(ltx, validationSeqNum))
    {
        return AddResult::ADD_STATUS_ERROR;
    }

    int64_t totalFees = getTotalFee(tx->getFeeSourceID());
    auto feeSource = stellar::loadAccount(ltx, tx->getFeeSourceID());
    if (INT64_MAX - totalFees < tx->getFeeBid() ||
        getAvailableBalance(ltx.loadHeader(), feeSource) <
            totalFees + tx->getFeeBid())
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return AddResult::ADD_STATUS_ERROR;
    }

    addFee(tx);

    auto& transactions = mPendingTransactions[tx->getSourceID()].mTransactions;
    auto transactionsIter = findTx(transactions, tx);
    if (transactionsIter != transactions.end())
    {
        transactionsIter->emplace_back(tx);
    }
    else
    {
        transactions.emplace_back(1, tx);
    }

    return AddResult::ADD_STATUS_PENDING;
}

size_t
TransactionQueue::countBanned(int index) const
{
    return mBannedTransactions[index].size();
}

TransactionQueue::AccountTxQueueInfo
TransactionQueue::getAccountTransactionQueueInfo(AccountID const& acc) const
{
    AccountTxQueueInfo res;

    auto pendingIter = mPendingTransactions.find(acc);
    if (pendingIter != mPendingTransactions.end())
    {
        auto const& transactions = pendingIter->second.mTransactions;
        res.mMaxSeq =
            transactions.empty() ? 0 : transactions.back().front()->getSeqNum();
        res.mAge = pendingIter->second.mAge;
    }
    res.mTotalFees = getTotalFee(acc);
    return res;
}

bool
TransactionQueue::isBanned(Hash const& hash) const
{
    return std::any_of(
        std::begin(mBannedTransactions), std::end(mBannedTransactions),
        [&](std::unordered_set<Hash> const& transactions) {
            return transactions.find(hash) != std::end(transactions);
        });
}

void
TransactionQueue::removeApplied(
    std::vector<TransactionFrameBasePtr> const& dropTxs)
{
    for (auto const& tx : dropTxs)
    {
        auto pendingIter = mPendingTransactions.find(tx->getSourceID());
        if (pendingIter == mPendingTransactions.end())
        {
            continue;
        }

        auto& transactions = pendingIter->second.mTransactions;
        auto txIter = findTx(transactions, tx);
        if (txIter == transactions.end())
        {
            continue;
        }

        pendingIter->second.mAge = 0;
        for (auto const& t : *txIter)
        {
            removeFee(t);
        }

        transactions.erase(txIter);
        if (transactions.empty())
        {
            mPendingTransactions.erase(pendingIter);
        }
    }
}

void
TransactionQueue::removeTrimmed(
    std::vector<TransactionFrameBasePtr> const& dropTxs)
{
    for (auto const& tx : dropTxs)
    {
        auto pendingIter = mPendingTransactions.find(tx->getSourceID());
        if (pendingIter == mPendingTransactions.end())
        {
            continue;
        }

        auto& transactions = pendingIter->second.mTransactions;
        auto txIter = findTx(transactions, tx);
        if (txIter == transactions.end())
        {
            continue;
        }

        auto iter =
            std::find_if(txIter->begin(), txIter->end(),
                         [&tx](TransactionFrameBasePtr const& t) {
                             return t->getFullHash() == tx->getFullHash();
                         });
        if (iter != txIter->end())
        {
            removeFee(*iter);
            std::swap(*iter, txIter->back());
            txIter->pop_back();
        }

        if (txIter->empty())
        {
            auto eraseStart = txIter;
            for (; txIter != transactions.end(); ++txIter)
            {
                for (auto const& t : *txIter)
                {
                    removeFee(t);
                }
            }
            transactions.erase(eraseStart, transactions.end());
        }
    }
}

void
TransactionQueue::shift()
{
    mBannedTransactions.pop_back();
    mBannedTransactions.emplace_front();
    auto& banned = mBannedTransactions.front();

    auto sizes = std::vector<int64_t>{};
    sizes.resize(mPendingDepth);

    auto iter = mPendingTransactions.begin();
    while (iter != mPendingTransactions.end())
    {
        if (mPendingDepth == ++(iter->second.mAge))
        {
            for (auto const& txs : iter->second.mTransactions)
            {
                for (auto const& t : txs)
                {
                    removeFee(t);
                    banned.emplace(t->getFullHash());
                }
            }
            iter = mPendingTransactions.erase(iter);
        }
        else
        {
            sizes[iter->second.mAge] += std::accumulate(
                iter->second.mTransactions.begin(),
                iter->second.mTransactions.end(), size_t(0),
                [](size_t lhs,
                   std::vector<TransactionFrameBasePtr> const& rhs) {
                    return lhs + rhs.size();
                });
            ++iter;
        }
    }

    for (auto i = 0; i < sizes.size(); i++)
    {
        mSizeByAge[i]->set_count(sizes[i]);
    }
}

TransactionQueue::AddResult
TransactionQueue::tryAdd(TransactionFrameBasePtr const& tx)
{
    if (isBanned(tx->getFullHash()))
    {
        return AddResult::ADD_STATUS_TRY_AGAIN_LATER;
    }

    auto pendingIter = mPendingTransactions.find(tx->getSourceID());
    if (pendingIter != mPendingTransactions.end())
    {
        auto& transactions = pendingIter->second.mTransactions;
        auto txIter = findTx(transactions, tx);
        if (txIter != transactions.end())
        {
            auto iter =
                std::find_if(txIter->begin(), txIter->end(),
                             [&tx](TransactionFrameBasePtr const& t) {
                                 return t->getFullHash() == tx->getFullHash();
                             });
            if (iter != txIter->end())
            {
                return AddResult::ADD_STATUS_DUPLICATE;
            }

            return validateAndMaybeAddTransaction(tx, tx->getSeqNum() - 1);
        }
        else if (!transactions.empty())
        {
            return validateAndMaybeAddTransaction(
                tx, transactions.back().front()->getSeqNum());
        }
    }
    return validateAndMaybeAddTransaction(tx, 0);
}

std::shared_ptr<TxSetFrame>
TransactionQueue::toTxSet(Hash const& lclHash) const
{
    auto result = std::make_shared<TxSetFrame>(lclHash);

    for (auto const& m : mPendingTransactions)
    {
        for (auto const& txs : m.second.mTransactions)
        {
            for (auto const& tx : txs)
            {
                result->add(tx);
            }
        }
    }

    return result;
}
}
