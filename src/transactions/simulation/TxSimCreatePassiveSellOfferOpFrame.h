// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/CreatePassiveSellOfferOpFrame.h"

namespace stellar
{
namespace txsimulation
{

class TxSimCreatePassiveSellOfferOpFrame : public CreatePassiveSellOfferOpFrame
{
    OperationResult mSimulationResult;
    uint32_t mCount;

  public:
    TxSimCreatePassiveSellOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx,
                                       OperationResult const& simulationResult,
                                       uint32_t partition);

    int64_t generateNewOfferID(LedgerTxnHeader& header) override;
};
}
}
