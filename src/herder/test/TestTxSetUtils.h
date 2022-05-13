// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetUtils.h"

namespace stellar
{

class TestTxSetUtils
{
  public:
    static TxSetFrameConstPtr
    makeNonValidatedTxSet(std::vector<TransactionFrameBasePtr> const& txs,
                          Hash const& networkID,
                          Hash const& previousLedgerHash);

    static TransactionSet
    makeTxSetXDR(std::vector<TransactionFrameBasePtr> const& txs,
                 Hash const& previousLedgerHash);

    static GeneralizedTransactionSet makeGeneralizedTxSetXDR(
        std::vector<std::pair<std::optional<int64_t>,
                              std::vector<TransactionFrameBasePtr>>> const&
            txsPerBaseFee,
        Hash const& previousLedgerHash);
    // static TxSetFrameConstPtr
    // static TxSetFrameConstPtr addTxs(TxSetFrameConstPtr txSet,
    //                                  TxSetFrame::Transactions const&
    // newTxs);

    // static TxSetFrameConstPtr
    // removeTxs(TxSetFrameConstPtr txSet,
    //           TxSetFrame::Transactions const& txsToRemove);

    // static TxSetFrameConstPtr makeIllSortedTxSet(Hash const& networkID,
    //                                              TxSetFrameConstPtr
    // goodTxSet);

}; // class TestTxSetUtils
} // namespace stellar