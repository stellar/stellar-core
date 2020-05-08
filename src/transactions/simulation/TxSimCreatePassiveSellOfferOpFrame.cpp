// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/simulation/TxSimCreatePassiveSellOfferOpFrame.h"
#include "TxSimUtils.h"
#include "ledger/LedgerTxn.h"

namespace stellar
{
namespace txsimulation
{

TxSimCreatePassiveSellOfferOpFrame::TxSimCreatePassiveSellOfferOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx,
    OperationResult const& simulationResult, uint32_t partition)
    : CreatePassiveSellOfferOpFrame(op, res, parentTx)
    , mSimulationResult(simulationResult)
    , mCount(partition)
{
}

int64_t
TxSimCreatePassiveSellOfferOpFrame::generateNewOfferID(LedgerTxnHeader& header)
{
    return generateScaledOfferID(mSimulationResult, mCount);
}
}
}
