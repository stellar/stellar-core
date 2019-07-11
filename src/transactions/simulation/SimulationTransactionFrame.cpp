// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/simulation/SimulationTransactionFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionUtils.h"
#include "transactions/simulation/SimulationMergeOpFrame.h"

namespace stellar
{

TransactionFramePtr
SimulationTransactionFrame::makeTransactionFromWire(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionResult simulationResult)
{
    TransactionFramePtr res = std::make_shared<SimulationTransactionFrame>(
        networkID, envelope, simulationResult);
    return res;
}

SimulationTransactionFrame::SimulationTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionResult simulationResult)
    : TransactionFrame(networkID, envelope), mSimulationResult(simulationResult)
{
}

std::shared_ptr<OperationFrame>
SimulationTransactionFrame::makeOperation(Operation const& op,
                                          OperationResult& res, size_t index)
{
    if (mEnvelope.tx.operations[index].body.type() != ACCOUNT_MERGE)
    {
        return OperationFrame::makeHelper(op, res, *this);
    }
    else
    {
        return std::make_shared<SimulationMergeOpFrame>(
            op, res, *this, mSimulationResult.result.results()[index]);
    }
}

bool
SimulationTransactionFrame::isTooEarly(LedgerTxnHeader const& header) const
{
    return mSimulationResult.result.code() == txTOO_EARLY;
}

bool
SimulationTransactionFrame::isTooLate(LedgerTxnHeader const& header) const
{
    return mSimulationResult.result.code() == txTOO_LATE;
}

bool
SimulationTransactionFrame::isBadSeq(int64_t seqNum) const
{
    return mSimulationResult.result.code() == txBAD_SEQ;
}

int64_t
SimulationTransactionFrame::getFee(LedgerHeader const& header,
                                   int64_t baseFee) const
{
    return mSimulationResult.feeCharged;
}

void
SimulationTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                             int64_t baseFee)
{
    mCachedAccount.reset();

    auto header = ltx.loadHeader();
    resetResults(header.current(), baseFee);

    auto sourceAccount = loadSourceAccount(ltx, header);
    if (!sourceAccount)
    {
        return;
    }
    auto& acc = sourceAccount.current().data.account();

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
    // in v10 we update sequence numbers during apply
    if (header.current().ledgerVersion <= 9)
    {
        acc.seqNum = mEnvelope.tx.seqNum;
    }
}
}
