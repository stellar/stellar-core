// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "util/UnorderedMap.h"
#include "xdr/Stellar-types.h"
#include <ledger/LedgerHashUtils.h>
#include <tuple>

namespace stellar
{

class AccountTransactionQueue : public TxStack
{
  public:
    AccountTransactionQueue(
        std::vector<TransactionFrameBasePtr> const& accountTxs);

    TransactionFrameBasePtr getTopTx() const override;
    bool empty() const override;
    void popTopTx() override;
    Resource getResources() const override;

    std::deque<TransactionFrameBasePtr> mTxs;

  private:
    uint32_t mNumOperations = 0;
    bool mIsSoroban;
};

class TxSetUtils
{
  public:
    static bool hashTxSorter(TransactionFrameBasePtr const& tx1,
                             TransactionFrameBasePtr const& tx2);

    static TxSetTransactions
    sortTxsInHashOrder(TxSetTransactions const& transactions);

    static std::vector<std::shared_ptr<AccountTransactionQueue>>
    buildAccountTxQueues(TxSetTransactions const& txs);

    // Returns transactions from a TxSet that are invalid. If
    // returnEarlyOnFirstInvalidTx is true, return immediately if an invalid
    // transaction is found (instead of finding all of them), this is useful for
    // checking if a TxSet is valid.
    static TxSetTransactions
    getInvalidTxList(TxSetTransactions const& txs, Application& app,
                     uint64_t lowerBoundCloseTimeOffset,
                     uint64_t upperBoundCloseTimeOffset,
                     bool returnEarlyOnFirstInvalidTx);

    static TxSetTransactions trimInvalid(TxSetTransactions const& txs,
                                         Application& app,
                                         uint64_t lowerBoundCloseTimeOffset,
                                         uint64_t upperBoundCloseTimeOffset,
                                         TxSetTransactions& invalidTxs);
}; // class TxSetUtils
} // namespace stellar
