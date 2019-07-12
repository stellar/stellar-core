// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TransactionQueue.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/HashOfHash.h"
#include "util/XDROperators.h"

#include <algorithm>
#include <lib/util/format.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <numeric>

namespace stellar
{

TransactionQueue::TransactionQueue(Application& app, int pendingDepth,
                                   int banDepth)
    : mApp(app), mPendingDepth(pendingDepth), mBannedTransactions(banDepth)
{
    for (auto i = 0; i < pendingDepth; i++)
    {
        mSizeByAge.emplace_back(&app.getMetrics().NewCounter(
            {"herder", "pending-txs", fmt::format("age{}", i)}));
    }
}

TransactionQueue::AddResult
TransactionQueue::tryAdd(TransactionFramePtr tx)
{
    if (isBanned(tx->getFullHash()))
    {
        return TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER;
    }

    if (contains(tx))
    {
        return TransactionQueue::AddResult::ADD_STATUS_DUPLICATE;
    }

    auto info = getAccountTransactionQueueInfo(tx->getSourceID());
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    if (!tx->checkValid(ltx, info.mMaxSeq))
    {
        return TransactionQueue::AddResult::ADD_STATUS_ERROR;
    }

    auto sourceAccount = stellar::loadAccount(ltx, tx->getSourceID());
    if (getAvailableBalance(ltx.loadHeader(), sourceAccount) - tx->getFeeBid() <
        info.mTotalFees)
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return TransactionQueue::AddResult::ADD_STATUS_ERROR;
    }

    auto& pendingForAccount = mPendingTransactions[tx->getSourceID()];
    mSizeByAge[pendingForAccount.mAge]->inc();
    pendingForAccount.mTotalFees += tx->getFeeBid();
    pendingForAccount.mTransactions.emplace_back(tx);
    return TransactionQueue::AddResult::ADD_STATUS_PENDING;
}

void
TransactionQueue::removeAndReset(
    std::vector<TransactionFramePtr> const& dropTxs)
{
    for (auto const& tx : dropTxs)
    {
        auto extracted = extract(tx, true);
        if (extracted.first != std::end(mPendingTransactions))
        {
            extracted.first->second.mAge = 0;
        }
    }
}

void
TransactionQueue::ban(std::vector<TransactionFramePtr> const& dropTxs)
{
    auto& bannedFront = mBannedTransactions.front();
    for (auto const& tx : dropTxs)
    {
        for (auto const& extracted : extract(tx, false).second)
        {
            bannedFront.insert(extracted->getFullHash());
        }
    }
}

bool
TransactionQueue::contains(TransactionFramePtr tx)
{
    return find(tx).first != std::end(mPendingTransactions);
}

TransactionQueue::FindResult
TransactionQueue::find(TransactionFramePtr const& tx)
{
    auto const& acc = tx->getSourceID();
    auto accIt = mPendingTransactions.find(acc);
    if (accIt == std::end(mPendingTransactions))
    {
        return {std::end(mPendingTransactions), {}};
    }

    auto& txs = accIt->second.mTransactions;
    auto txIt =
        std::find_if(std::begin(txs), std::end(txs), [&](auto const& t) {
            return tx->getSeqNum() == t->getSeqNum();
        });
    if (txIt == std::end(txs))
    {
        return {std::end(mPendingTransactions), {}};
    }

    if ((*txIt)->getFullHash() != tx->getFullHash())
    {
        return {std::end(mPendingTransactions), {}};
    }

    return {accIt, txIt};
}

TransactionQueue::ExtractResult
TransactionQueue::extract(TransactionFramePtr const& tx, bool keepBacklog)
{
    auto it = find(tx);
    auto accIt = it.first;
    if (accIt == std::end(mPendingTransactions))
    {
        return {std::end(mPendingTransactions), {}};
    }

    auto& txs = accIt->second.mTransactions;
    auto txIt = it.second;

    auto txRemoveEnd = txIt + 1;
    if (!keepBacklog)
    {
        // remove everything passed tx
        txRemoveEnd = std::end(txs);
    }
    auto removedFeeBid =
        std::accumulate(txIt, txRemoveEnd, int64_t{0},
                        [](int64_t fee, TransactionFramePtr const& tx) {
                            return fee + tx->getFeeBid();
                        });
    accIt->second.mTotalFees -= removedFeeBid;

    auto movedTxs = std::vector<TransactionFramePtr>{};
    std::move(txIt, txRemoveEnd, std::back_inserter(movedTxs));
    txs.erase(txIt, txRemoveEnd);

    if (accIt->second.mTransactions.empty())
    {
        mPendingTransactions.erase(accIt);
        accIt = std::end(mPendingTransactions);
    }

    return {accIt, std::move(movedTxs)};
}

TransactionQueue::AccountTxQueueInfo
TransactionQueue::getAccountTransactionQueueInfo(
    AccountID const& accountID) const
{
    auto i = mPendingTransactions.find(accountID);
    if (i == std::end(mPendingTransactions))
    {
        return {0, 0, 0};
    }

    return {i->second.mTransactions.back()->getSeqNum(), i->second.mTotalFees,
            i->second.mAge};
}

void
TransactionQueue::shift()
{
    mBannedTransactions.pop_back();
    mBannedTransactions.emplace_front();

    auto sizes = std::vector<int64_t>{};
    sizes.resize(mPendingDepth);

    auto& bannedFront = mBannedTransactions.front();
    auto end = std::end(mPendingTransactions);
    auto it = std::begin(mPendingTransactions);
    while (it != end)
    {
        if (mPendingDepth == ++(it->second.mAge))
        {
            for (auto const& toBan : it->second.mTransactions)
            {
                bannedFront.insert(toBan->getFullHash());
            }

            it = mPendingTransactions.erase(it);
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
        [&](std::unordered_set<Hash> const& transactions) {
            return transactions.find(hash) != std::end(transactions);
        });
}

std::shared_ptr<TxSetFrame>
TransactionQueue::toTxSet(Hash const& lclHash) const
{
    auto result = std::make_shared<TxSetFrame>(lclHash);

    for (auto const& m : mPendingTransactions)
    {
        for (auto const& tx : m.second.mTransactions)
        {
            result->add(tx);
        }
    }

    return result;
}

bool
operator==(TransactionQueue::AccountTxQueueInfo const& x,
           TransactionQueue::AccountTxQueueInfo const& y)
{
    return x.mMaxSeq == y.mMaxSeq && x.mTotalFees == y.mTotalFees;
}
}
