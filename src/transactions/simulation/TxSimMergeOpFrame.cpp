// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/simulation/TxSimMergeOpFrame.h"

namespace stellar
{
namespace txsimulation
{

TxSimMergeOpFrame::TxSimMergeOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx,
                                     OperationResult const& simulationResult)
    : MergeOpFrame(op, res, parentTx), mSimulationResult(simulationResult)
{
}

bool
TxSimMergeOpFrame::isSeqnumTooFar(LedgerTxnHeader const& header,
                                  AccountEntry const& sourceAccount)
{
    if (mSimulationResult.code() == opINNER)
    {
        auto code = mSimulationResult.tr().accountMergeResult().code();
        return code == ACCOUNT_MERGE_SEQNUM_TOO_FAR;
    }
    return false;
}
}
}
