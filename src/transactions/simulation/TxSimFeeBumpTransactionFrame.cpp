// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSimFeeBumpTransactionFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionUtils.h"
#include "transactions/simulation/TxSimTransactionFrame.h"

namespace stellar
{
namespace txsimulation
{

TxSimFeeBumpTransactionFrame::TxSimFeeBumpTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionResult simulationResult, uint32_t partition)
    : FeeBumpTransactionFrame(
          networkID, envelope,
          std::make_shared<TxSimTransactionFrame>(
              networkID, FeeBumpTransactionFrame::convertInnerTxToV1(envelope),
              simulationResult, partition))
    , mSimulationResult(simulationResult)
{
}

int64_t
TxSimFeeBumpTransactionFrame::getFee(const stellar::LedgerHeader& header,
                                     int64_t baseFee, bool applying) const
{
    return mSimulationResult.feeCharged;
}

void
TxSimFeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                               int64_t baseFee)
{
    resetResults(ltx.loadHeader().current(), baseFee, true);

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!feeSource)
    {
        return;
    }
    auto& acc = feeSource.current().data.account();

    auto header = ltx.loadHeader();
    int64_t& fee = getResult().feeCharged;
    if (fee > 0)
    {
        fee = std::min(acc.balance, fee);
        // Note: TransactionUtil addBalance checks that reserve plus liabilities
        // are respected. In this case, we allow it to fall below that since it
        // will be caught later in commonValid.
        stellar::addBalance(acc.balance, -fee);
        header.current().feePool += fee;
    }
}
}
}