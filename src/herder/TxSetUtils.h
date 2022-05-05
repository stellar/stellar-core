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
class TxSetUtils
{
  public:
    static bool isValidHashOrder(TxSetFrame::Transactions const& txs);

    static TxSetFrame::Transactions
    extractTxsFromXdrSet(Hash const& networkID, TransactionSet const& xdrSet);

    static TxSetFrame::Transactions
    sortTxsInHashOrder(TxSetFrame::Transactions const& transactions);

    static Hash
    computeContentsHash(Hash const& previousLedgerHash,
                        TxSetFrame::Transactions const& txsInHashOrder);

    static UnorderedMap<AccountID, TxSetFrame::AccountTransactionQueue>
    buildAccountTxQueues(TxSetFrame const& txSet);

    static TxSetFrameConstPtr surgePricingFilter(TxSetFrameConstPtr txSet,
                                                 Application& app);

    // Returns transactions from a TxSet that are invalid. If
    // returnEarlyOnFirstInvalidTx is true, return immediately if an invalid
    // transaction is found (instead of finding all of them), this is useful for
    // checking if a TxSet is valid.
    static TxSetFrame::Transactions
    getInvalidTxList(Application& app, TxSetFrame const& txSet,
                     uint64_t lowerBoundCloseTimeOffset,
                     uint64_t upperBoundCloseTimeOffset,
                     bool returnEarlyOnFirstInvalidTx);

    // For caching TxSet validity. Consist of {lcl.hash, txSetHash,
    // lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset}
    using TxSetValidityKey = std::tuple<Hash, Hash, uint64_t, uint64_t>;

    class TxSetValidityKeyHash
    {
      public:
        size_t
        operator()(TxSetValidityKey const& key) const
        {
            size_t res = std::hash<Hash>()(std::get<0>(key));
            hashMix(res, std::hash<Hash>()(std::get<1>(key)));
            hashMix(res, std::get<2>(key));
            hashMix(res, std::get<3>(key));
            return res;
        }
    };

  private:
    static TxSetFrameConstPtr
    removeTxs(TxSetFrameConstPtr txSet,
              TxSetFrame::Transactions const& txsToRemove);

    friend class TestTxSetUtils;
}; // class TxSetUtils
} // namespace stellar