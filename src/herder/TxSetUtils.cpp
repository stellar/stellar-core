// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
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
namespace TxSetUtils
{

bool
HashTxSorter(TransactionFrameBasePtr const& tx1,
             TransactionFrameBasePtr const& tx2)
{
    // need to use the hash of whole tx here since multiple txs could have
    // the same Contents
    return tx1->getFullHash() < tx2->getFullHash();
}

// order the txset correctly
// must take into account multiple tx from same account
TxSetFrame::Transactions
sortTxsInHashOrder(TxSetFrame::Transactions const& transactions)
{
    ZoneScoped;
    TxSetFrame::Transactions sortedTxs(transactions);
    std::sort(sortedTxs.begin(), sortedTxs.end(), HashTxSorter);
    return sortedTxs;
}

TxSetFrame::Transactions
sortTxsInHashOrder(Hash const& networkID, TransactionSet const& xdrSet)
{
    ZoneScoped;
    TxSetFrame::Transactions txs;
    txs.reserve(xdrSet.txs.size());
    std::transform(xdrSet.txs.cbegin(), xdrSet.txs.cend(),
                   std::back_inserter(txs),
                   [&](TransactionEnvelope const& env) {
                       return TransactionFrameBase::makeTransactionFromWire(
                           networkID, env);
                   });
    return sortTxsInHashOrder(txs);
}

Hash
computeContentsHash(Hash const& previousLedgerHash,
                    TxSetFrame::Transactions const& txsInHashOrder)
{
    ZoneScoped;
    SHA256 hasher;
    hasher.add(previousLedgerHash);
    for (unsigned int n = 0; n < txsInHashOrder.size(); n++)
    {
        hasher.add(xdr::xdr_to_opaque(txsInHashOrder[n]->getEnvelope()));
    }
    return hasher.finish();
}

UnorderedMap<AccountID, TxSetFrame::AccountTransactionQueue>
buildAccountTxQueues(TxSetFrame const& txSet)
{
    ZoneScoped;
    UnorderedMap<AccountID, TxSetFrame::AccountTransactionQueue> actTxQueueMap;
    for (auto const& tx : txSet.getTxsInHashOrder())
    {
        auto id = tx->getSourceID();
        auto it = actTxQueueMap.find(id);
        if (it == actTxQueueMap.end())
        {
            auto d = std::make_pair(id, TxSetFrame::AccountTransactionQueue{});
            auto r = actTxQueueMap.insert(d);
            it = r.first;
        }
        it->second.emplace_back(tx);
    }

    for (auto& am : actTxQueueMap)
    {
        // sort each in sequence number order
        std::sort(am.second.begin(), am.second.end(),
                  [](TransactionFrameBasePtr const& tx1,
                     TransactionFrameBasePtr const& tx2) {
                      return tx1->getSeqNum() < tx2->getSeqNum();
                  });
    }
    return actTxQueueMap;
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

TxSetFrameConstPtr
surgePricingFilter(TxSetFrameConstPtr txSet, Application& app)
{
    ZoneScoped;
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto header = ltx.loadHeader();

    bool maxIsOps = protocolVersionStartsFrom(header.current().ledgerVersion,
                                              ProtocolVersion::V_11);

    size_t opsLeft = app.getLedgerManager().getLastMaxTxSetSizeOps();

    auto curSizeOps =
        maxIsOps ? txSet->sizeOp() : (txSet->sizeTx() * MAX_OPS_PER_TX);
    if (curSizeOps > opsLeft)
    {
        CLOG_WARNING(Herder, "surge pricing in effect! {} > {}", curSizeOps,
                     opsLeft);

        auto actTxQueueMap = buildAccountTxQueues(*txSet);

        std::priority_queue<TxSetFrame::AccountTransactionQueue*,
                            std::vector<TxSetFrame::AccountTransactionQueue*>,
                            SurgeCompare>
            surgeQueue;

        for (auto& am : actTxQueueMap)
        {
            surgeQueue.push(&am.second);
        }

        TxSetFrame::Transactions updatedSet;
        updatedSet.reserve(txSet->sizeTx());
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

        return std::make_shared<TxSetFrame const>(txSet->previousLedgerHash(),
                                                  updatedSet);
    }
    return txSet;
}

TxSetFrame::Transactions
getInvalidTxList(Application& app, TxSetFrame const& txSet,
                 uint64_t lowerBoundCloseTimeOffset,
                 uint64_t upperBoundCloseTimeOffset,
                 bool returnEarlyOnFirstInvalidTx)
{
    ZoneScoped;
    LedgerTxn ltx(app.getLedgerTxnRoot());

    if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        // This is done so minSeqLedgerGap is validated against the next
        // ledgerSeq, which is what will be used at apply time
        ltx.loadHeader().current().ledgerSeq =
            app.getLedgerManager().getLastClosedLedgerNum() + 1;
    }

    UnorderedMap<AccountID, int64_t> accountFeeMap;
    TxSetFrame::Transactions invalidTxs;

    auto accountTxMap = buildAccountTxQueues(txSet);
    for (auto& kv : accountTxMap)
    {
        int64_t lastSeq = 0;
        auto iter = kv.second.begin();
        while (iter != kv.second.end())
        {
            auto tx = *iter;
            // In addition to checkValid, we also want to make sure that all but
            // the transaction with the lowest seqNum on a given sourceAccount
            // do not have minSeqAge and minSeqLedgerGap set
            bool minSeqCheckIsInvalid =
                iter != kv.second.begin() &&
                (tx->getMinSeqAge() != 0 || tx->getMinSeqLedgerGap() != 0);
            if (minSeqCheckIsInvalid ||
                !tx->checkValid(ltx, lastSeq, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset))
            {
                invalidTxs.emplace_back(tx);
                iter = kv.second.erase(iter);
                if (returnEarlyOnFirstInvalidTx)
                {
                    if (minSeqCheckIsInvalid)
                    {
                        CLOG_DEBUG(Herder,
                                   "minSeqAge or minSeqLedgerGap set on tx "
                                   "without lowest seqNum. tx: {}",
                                   xdr_to_string(tx->getEnvelope(),
                                                 "TransactionEnvelope"));
                    }
                    else
                    {
                        CLOG_DEBUG(
                            Herder,
                            "Got bad txSet: {} tx invalid lastSeq:{} tx: {} "
                            "result: {}",
                            hexAbbrev(txSet.previousLedgerHash()), lastSeq,
                            xdr_to_string(tx->getEnvelope(),
                                          "TransactionEnvelope"),
                            tx->getResultCode());
                    }
                    return invalidTxs;
                }
            }
            else // update the account fee map
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
                while (iter != kv.second.end())
                {
                    invalidTxs.emplace_back(*iter);
                    ++iter;
                }
                if (returnEarlyOnFirstInvalidTx)
                {
                    CLOG_DEBUG(Herder,
                               "Got bad txSet: {} account can't pay fee tx: {}",
                               hexAbbrev(txSet.previousLedgerHash()),
                               xdr_to_string(tx->getEnvelope(),
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

// Target use case is to remove a subset of invalid transactions from a TxSet.
// I.e. txSet.size() >= txsToRemove.size()
TxSetFrameConstPtr
removeTxs(TxSetFrameConstPtr txSet, TxSetFrame::Transactions const& txsToRemove)
{
    // hashmap from the full txSet
    std::unordered_map<Hash, TransactionFrameBasePtr> fullTxSetHashMap;
    fullTxSetHashMap.reserve(txSet->sizeTx());
    std::transform(txSet->getTxsInHashOrder().cbegin(),
                   txSet->getTxsInHashOrder().cend(),
                   std::inserter(fullTxSetHashMap, fullTxSetHashMap.end()),
                   [](TransactionFrameBasePtr const& tx) {
                       return std::pair<Hash, TransactionFrameBasePtr>{
                           tx->getFullHash(), tx};
                   });

    // candidate tx hashes to be removed
    std::unordered_set<Hash> removeTxHashSet;
    removeTxHashSet.reserve(txsToRemove.size());
    std::transform(
        txsToRemove.cbegin(), txsToRemove.cend(),
        std::inserter(removeTxHashSet, removeTxHashSet.end()),
        [](TransactionFrameBasePtr const& tx) { return tx->getFullHash(); });

    // remove txs
    for (auto it = fullTxSetHashMap.begin(); it != fullTxSetHashMap.end();)
    {
        if (removeTxHashSet.find(it->first) != removeTxHashSet.end())
        {
            it = fullTxSetHashMap.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // get back remaining txs
    TxSetFrame::Transactions txs;
    txs.reserve(fullTxSetHashMap.size());
    std::transform(fullTxSetHashMap.cbegin(), fullTxSetHashMap.cend(),
                   std::back_inserter(txs),
                   [](std::pair<Hash, TransactionFrameBasePtr> const& p) {
                       return p.second;
                   });

    return std::make_shared<TxSetFrame const>(txSet->previousLedgerHash(), txs);
}

TxSetFrameConstPtr
addTxs(TxSetFrameConstPtr txSet, TxSetFrame::Transactions const& newTxs)
{
    auto updated = txSet->getTxsInHashOrder();
    updated.insert(updated.end(), newTxs.begin(), newTxs.end());
    return std::make_shared<TxSetFrame const>(txSet->previousLedgerHash(),
                                              updated);
}

} // namespace TxSetUtils
} // namespace stellar