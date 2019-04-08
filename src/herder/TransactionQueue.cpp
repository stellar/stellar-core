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

namespace stellar
{

static uint64_t
countTxs(TransactionQueue::AccountTxMap const& acc)
{
    uint64_t sz = 0;
    for (auto const& a : acc)
    {
        sz += a.second->mTransactions.size();
    }
    return sz;
}

static std::shared_ptr<TransactionQueue::TxMap>
findOrAdd(TransactionQueue::AccountTxMap& acc, AccountID const& aid)
{
    std::shared_ptr<TransactionQueue::TxMap> txmap;
    auto i = acc.find(aid);
    if (i == acc.end())
    {
        txmap = std::make_shared<TransactionQueue::TxMap>();
        acc.emplace(std::make_pair(aid, txmap));
    }
    else
    {
        txmap = i->second;
    }
    return txmap;
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
TransactionQueue::TxMap::addTx(TransactionFramePtr tx)
{
    auto const& h = tx->getFullHash();
    if (mTransactions.find(h) != mTransactions.end())
    {
        return;
    }
    mTransactions.emplace(std::make_pair(h, tx));
    mCurrentState.mMaxSeq = std::max(tx->getSeqNum(), mCurrentState.mMaxSeq);
    mCurrentState.mTotalFees += tx->getFeeBid();
}

void
TransactionQueue::TxMap::recalculate()
{
    mCurrentState.mMaxSeq = 0;
    mCurrentState.mTotalFees = 0;
    for (auto const& pair : mTransactions)
    {
        mCurrentState.mMaxSeq =
            std::max(pair.second->getSeqNum(), mCurrentState.mMaxSeq);
        mCurrentState.mTotalFees += pair.second->getFeeBid();
    }
}

TransactionQueue::TransactionQueue(Application& app, int pendingDepth,
                                   int banDepth)
    : mApp(app)
    , mPendingTransactions(pendingDepth)
    , mBannedTransactions(banDepth)
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
        return TransactionQueue::AddResult::STATUS_TRY_AGAIN_LATER;
    }

    if (contains(tx))
    {
        return TransactionQueue::AddResult::STATUS_DUPLICATE;
    }

    auto state = getAccountTransactionQueueState(tx->getSourceID());
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    if (!tx->checkValid(ltx, state.mMaxSeq))
    {
        return TransactionQueue::AddResult::STATUS_ERROR;
    }

    auto sourceAccount = stellar::loadAccount(ltx, tx->getSourceID());
    if (getAvailableBalance(ltx.loadHeader(), sourceAccount) - tx->getFeeBid() <
        state.mTotalFees)
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return TransactionQueue::AddResult::STATUS_ERROR;
    }

    auto map = findOrAdd(mPendingTransactions[0], tx->getSourceID());
    map->addTx(tx);

    return TransactionQueue::AddResult::STATUS_PENDING;
}

void
TransactionQueue::remove(std::vector<TransactionFramePtr> const& dropTxs)
{
    for (auto& m : mPendingTransactions)
    {
        if (m.empty())
        {
            continue;
        }

        std::set<std::shared_ptr<TxMap>> toRecalculate;

        for (auto const& tx : dropTxs)
        {
            auto const& acc = tx->getSourceID();
            auto const& txID = tx->getFullHash();
            auto i = m.find(acc);
            if (i != m.end())
            {
                auto& txs = i->second->mTransactions;
                auto j = txs.find(txID);
                if (j != txs.end())
                {
                    txs.erase(j);
                    if (txs.empty())
                    {
                        m.erase(i);
                    }
                    else
                    {
                        toRecalculate.insert(i->second);
                    }
                }
            }
        }

        for (auto txm : toRecalculate)
        {
            txm->recalculate();
        }
    }
}

bool
TransactionQueue::contains(TransactionFramePtr tx) const
{
    return std::any_of(std::begin(mPendingTransactions),
                       std::end(mPendingTransactions),
                       [&](AccountTxMap const& map) {
                           auto txMap = map.find(tx->getSourceID());
                           if (txMap == map.end())
                           {
                               return false;
                           }

                           auto& transactions = txMap->second->mTransactions;
                           return transactions.find(tx->getFullHash()) !=
                                  std::end(transactions);
                       });
}

AccountTransactionsQueueState
TransactionQueue::getAccountTransactionQueueState(
    AccountID const& accountID) const
{
    AccountTransactionsQueueState state;

    for (auto& map : mPendingTransactions)
    {
        auto i = map.find(accountID);
        if (i != map.end())
        {
            auto& txmap = i->second;
            state.mTotalFees += txmap->mCurrentState.mTotalFees;
            state.mMaxSeq =
                std::max(state.mMaxSeq, txmap->mCurrentState.mMaxSeq);
        }
    }

    return state;
}

void
TransactionQueue::shift()
{
    mBannedTransactions.pop_back();
    mBannedTransactions.emplace_front();

    auto& bannedFront = mBannedTransactions.front();
    for (auto const& map : mPendingTransactions.back())
    {
        for (auto const& toBan : map.second->mTransactions)
        {
            bannedFront.insert(toBan.first);
        }
    }

    mPendingTransactions.pop_back();
    mPendingTransactions.emplace_front();

    for (auto i = 0; i < mPendingTransactions.size(); i++)
    {
        mSizeByAge[i]->set_count(countTxs(mPendingTransactions[i]));
    }
}

int
TransactionQueue::countBanned(int index) const
{
    return mBannedTransactions[index].size();
}

std::shared_ptr<TxSetFrame>
TransactionQueue::toTxSet(Hash const& lclHash) const
{
    auto result = std::make_shared<TxSetFrame>(lclHash);

    for (auto const& m : mPendingTransactions)
    {
        for (auto const& pair : m)
        {
            for (auto const& tx : pair.second->mTransactions)
            {
                result->add(tx.second);
            }
        }
    }

    return result;
}

bool
operator==(AccountTransactionsQueueState const& x,
           AccountTransactionsQueueState const& y)
{
    return x.mMaxSeq == y.mMaxSeq && x.mTotalFees == y.mTotalFees;
}
}
