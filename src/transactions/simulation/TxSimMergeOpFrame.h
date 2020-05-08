// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/MergeOpFrame.h"

namespace stellar
{
namespace txsimulation
{

class TxSimMergeOpFrame : public MergeOpFrame
{
    OperationResult mSimulationResult;

  public:
    TxSimMergeOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx,
                      OperationResult const& simulationResult);

    bool isSeqnumTooFar(LedgerTxnHeader const& header,
                        AccountEntry const& sourceAccount) override;
};
}
}
