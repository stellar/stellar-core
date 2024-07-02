// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetUtils.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/UnorderedSet.h"
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
// Target use case is to remove a subset of invalid transactions from a TxSet.
// I.e. txSet.size() >= txsToRemove.size()
TxSetTransactions
removeTxs(TxSetTransactions const& txs, TxSetTransactions const& txsToRemove)
{
    UnorderedSet<Hash> txsToRemoveSet;
    txsToRemoveSet.reserve(txsToRemove.size());
    std::transform(
        txsToRemove.cbegin(), txsToRemove.cend(),
        std::inserter(txsToRemoveSet, txsToRemoveSet.end()),
        [](TransactionFrameBasePtr const& tx) { return tx->getFullHash(); });

    TxSetTransactions newTxs;
    newTxs.reserve(txs.size() - txsToRemove.size());
    for (auto const& tx : txs)
    {
        if (txsToRemoveSet.find(tx->getFullHash()) == txsToRemoveSet.end())
        {
            newTxs.emplace_back(tx);
        }
    }

    return newTxs;
}
} // namespace

AccountTransactionQueue::AccountTransactionQueue(
    std::vector<TransactionFrameBasePtr> const& accountTxs)
    : mTxs(accountTxs.begin(), accountTxs.end())
{
    releaseAssert(!mTxs.empty());
    std::sort(mTxs.begin(), mTxs.end(),
              [](TransactionFrameBasePtr const& tx1,
                 TransactionFrameBasePtr const& tx2) {
                  return tx1->getSeqNum() < tx2->getSeqNum();
              });
    for (auto const& tx : accountTxs)
    {
        mNumOperations += tx->getNumOperations();
    }

    if (mTxs[0]->isSoroban())
    {
        releaseAssert(mTxs.size() == 1);
        mIsSoroban = true;
    }
    else
    {
        mIsSoroban = false;
    }
}

TransactionFrameBasePtr
AccountTransactionQueue::getTopTx() const
{
    releaseAssert(!mTxs.empty());
    return mTxs.front();
}

bool
AccountTransactionQueue::empty() const
{
    return mTxs.empty();
}

void
AccountTransactionQueue::popTopTx()
{
    releaseAssert(!mTxs.empty());
    mNumOperations -= mTxs.front()->getNumOperations();
    mTxs.pop_front();
}

Resource
AccountTransactionQueue::getResources() const
{
    return empty() ? Resource::makeEmpty(mIsSoroban
                                             ? NUM_SOROBAN_TX_RESOURCES
                                             : NUM_CLASSIC_TX_BYTES_RESOURCES)
                   : getTopTx()->getResources(true);
}

bool
TxSetUtils::hashTxSorter(TransactionFrameBasePtr const& tx1,
                         TransactionFrameBasePtr const& tx2)
{
    // need to use the hash of whole tx here since multiple txs could have
    // the same Contents
    return tx1->getFullHash() < tx2->getFullHash();
}

TxSetTransactions
TxSetUtils::sortTxsInHashOrder(TxSetTransactions const& transactions)
{
    ZoneScoped;
    TxSetTransactions sortedTxs(transactions);
    std::sort(sortedTxs.begin(), sortedTxs.end(), TxSetUtils::hashTxSorter);
    return sortedTxs;
}

std::vector<std::shared_ptr<AccountTransactionQueue>>
TxSetUtils::buildAccountTxQueues(TxSetTransactions const& txs)
{
    ZoneScoped;
    UnorderedMap<AccountID, std::vector<TransactionFrameBasePtr>> actTxMap;

    for (auto& tx : txs)
    {
        auto id = tx->getSourceID();
        auto it =
            actTxMap.emplace(id, std::vector<TransactionFrameBasePtr>()).first;
        it->second.emplace_back(tx);
    }

    std::vector<std::shared_ptr<AccountTransactionQueue>> queues;
    for (auto const& [_, actTxs] : actTxMap)
    {
        queues.emplace_back(std::make_shared<AccountTransactionQueue>(actTxs));
    }
    return queues;
}

TxSetTransactions
TxSetUtils::getInvalidTxList(TxSetTransactions const& txs, Application& app,
                             uint64_t lowerBoundCloseTimeOffset,
                             uint64_t upperBoundCloseTimeOffset,
                             bool returnEarlyOnFirstInvalidTx)
{
    ZoneScoped;
    LedgerTxn ltx(app.getLedgerTxnRoot(), /* shouldUpdateLastModified */ true,
                  TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        // This is done so minSeqLedgerGap is validated against the next
        // ledgerSeq, which is what will be used at apply time
        ltx.loadHeader().current().ledgerSeq =
            app.getLedgerManager().getLastClosedLedgerNum() + 1;
    }

    UnorderedMap<AccountID, int64_t> accountFeeMap;
    TxSetTransactions invalidTxs;

    auto accountTxQueues = buildAccountTxQueues(txs);
    for (auto& accountQueue : accountTxQueues)
    {
        int64_t lastSeq = 0;
        auto iter = accountQueue->mTxs.begin();
        while (iter != accountQueue->mTxs.end())
        {
            auto tx = *iter;
            // In addition to checkValid, we also want to make sure that all but
            // the transaction with the lowest seqNum on a given sourceAccount
            // do not have minSeqAge and minSeqLedgerGap set
            bool minSeqCheckIsInvalid =
                iter != accountQueue->mTxs.begin() &&
                (tx->getMinSeqAge() != 0 || tx->getMinSeqLedgerGap() != 0);

            // Only call checkValid if we passed the seqNum check
            MutableTxResultPtr txResult{};
            if (!minSeqCheckIsInvalid)
            {
                txResult =
                    tx->checkValid(app, ltx, lastSeq, lowerBoundCloseTimeOffset,
                                   upperBoundCloseTimeOffset);
                releaseAssertOrThrow(txResult);
            }

            if (minSeqCheckIsInvalid || !txResult->isSuccess())
            {
                invalidTxs.emplace_back(tx);
                iter = accountQueue->mTxs.erase(iter);
                if (returnEarlyOnFirstInvalidTx)
                {
                    if (minSeqCheckIsInvalid)
                    {
                        CLOG_DEBUG(Herder,
                                   "minSeqAge or minSeqLedgerGap set on tx "
                                   "without lowest seqNum. tx: {}",
                                   xdrToCerealString(tx->getEnvelope(),
                                                     "TransactionEnvelope"));
                    }
                    else
                    {
                        CLOG_DEBUG(
                            Herder,
                            "Got bad txSet: tx invalid lastSeq:{} tx: {} "
                            "result: {}",
                            lastSeq,
                            xdrToCerealString(tx->getEnvelope(),
                                              "TransactionEnvelope"),
                            txResult->getResultCode());
                    }
                    return invalidTxs;
                }
            }
            else // update the account fee map
            {
                lastSeq = tx->getSeqNum();
                int64_t& accFee = accountFeeMap[tx->getFeeSourceID()];
                if (INT64_MAX - accFee < tx->getFullFee())
                {
                    accFee = INT64_MAX;
                }
                else
                {
                    accFee += tx->getFullFee();
                }
                ++iter;
            }
        }
    }

    auto header = ltx.loadHeader();
    for (auto& accountQueue : accountTxQueues)
    {
        auto iter = accountQueue->mTxs.begin();
        while (iter != accountQueue->mTxs.end())
        {
            auto tx = *iter;
            auto feeSource = stellar::loadAccount(ltx, tx->getFeeSourceID());
            auto totFee = accountFeeMap[tx->getFeeSourceID()];
            if (getAvailableBalance(header, feeSource) < totFee)
            {
                while (iter != accountQueue->mTxs.end())
                {
                    invalidTxs.emplace_back(*iter);
                    ++iter;
                }
                if (returnEarlyOnFirstInvalidTx)
                {
                    CLOG_DEBUG(Herder,
                               "Got bad txSet: account can't pay fee tx: {}",
                               xdrToCerealString(tx->getEnvelope(),
                                                 "TransactionEnvelope"));
                    return invalidTxs;
                }
            }
            else
            {
                ++iter;
            }
        }
    }

    return invalidTxs;
}

TxSetTransactions
TxSetUtils::trimInvalid(TxSetTransactions const& txs, Application& app,
                        uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset,
                        TxSetTransactions& invalidTxs)
{
    invalidTxs = getInvalidTxList(txs, app, lowerBoundCloseTimeOffset,
                                  upperBoundCloseTimeOffset, false);
    return removeTxs(txs, invalidTxs);
}

} // namespace stellar
