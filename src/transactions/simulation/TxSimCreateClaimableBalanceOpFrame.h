// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/CreateClaimableBalanceOpFrame.h"

namespace stellar
{
namespace txsimulation
{

class TxSimCreateClaimableBalanceOpFrame : public CreateClaimableBalanceOpFrame
{
    OperationResult mSimulationResult;
    uint32_t mCount;

  public:
    TxSimCreateClaimableBalanceOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx,
                                       uint32_t index,
                                       OperationResult const& simulationResult,
                                       uint32_t partition);
    Hash getBalanceID() override;
};
}
}
