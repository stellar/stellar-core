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
TxSetFrame::Transactions
removeTxs(TxSetFrame::Transactions const& txs,
          TxSetFrame::Transactions const& txsToRemove)
{
    UnorderedSet<Hash> txsToRemoveSet;
    txsToRemoveSet.reserve(txsToRemove.size());
    std::transform(
        txsToRemove.cbegin(), txsToRemove.cend(),
        std::inserter(txsToRemoveSet, txsToRemoveSet.end()),
        [](TransactionFrameBasePtr const& tx) { return tx->getFullHash(); });

    TxSetFrame::Transactions newTxs;
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

bool
TxSetUtils::hashTxSorter(TransactionFrameBasePtr const& tx1,
                         TransactionFrameBasePtr const& tx2)
{
    // need to use the hash of whole tx here since multiple txs could have
    // the same Contents
    return tx1->getFullHash() < tx2->getFullHash();
}

TxSetFrame::Transactions
TxSetUtils::sortTxsInHashOrder(TxSetFrame::Transactions const& transactions)
{
    ZoneScoped;
    TxSetFrame::Transactions sortedTxs(transactions);
    std::sort(sortedTxs.begin(), sortedTxs.end(), TxSetUtils::hashTxSorter);
    return sortedTxs;
}

UnorderedMap<AccountID, TxSetFrame::AccountTransactionQueue>
TxSetUtils::buildAccountTxQueues(TxSetFrame::Transactions const& txs)
{
    ZoneScoped;
    UnorderedMap<AccountID, TxSetFrame::AccountTransactionQueue> actTxQueueMap;
    for (auto const& tx : txs)
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

TxSetFrame::Transactions
TxSetUtils::getInvalidTxList(TxSetFrame::Transactions const& txs,
                             Application& app,
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
    TxSetFrame::Transactions invalidTxs;

    auto accountTxMap = buildAccountTxQueues(txs);
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
                            "Got bad txSet: tx invalid lastSeq:{} tx: {} "
                            "result: {}",
                            lastSeq,
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
                               "Got bad txSet: account can't pay fee tx: {}",
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

TxSetFrame::Transactions
TxSetUtils::trimInvalid(TxSetFrame::Transactions const& txs, Application& app,
                        uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset,
                        TxSetFrame::Transactions& invalidTxs)
{
    invalidTxs = getInvalidTxList(txs, app, lowerBoundCloseTimeOffset,
                                  upperBoundCloseTimeOffset, false);
    return removeTxs(txs, invalidTxs);
}

} // namespace stellar
