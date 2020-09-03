// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/simulation/TxSimCreateClaimableBalanceOpFrame.h"
#include "TxSimUtils.h"

namespace stellar
{
namespace txsimulation
{

TxSimCreateClaimableBalanceOpFrame::TxSimCreateClaimableBalanceOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx,
    uint32_t index, OperationResult const& simulationResult, uint32_t partition)
    : CreateClaimableBalanceOpFrame(op, res, parentTx, index)
    , mSimulationResult(simulationResult)
    , mCount(partition)
{
}

Hash
TxSimCreateClaimableBalanceOpFrame::getBalanceID()
{
    return generateScaledClaimableBalanceID(mSimulationResult, mCount);
}
}
}
