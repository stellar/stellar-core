// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/simulation/TxSimTransactionFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionUtils.h"
#include "transactions/simulation/TxSimMergeOpFrame.h"

namespace stellar
{

TransactionFramePtr
TxSimTransactionFrame::makeTransactionFromWire(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionResult simulationResult)
{
    TransactionFramePtr res = std::make_shared<TxSimTransactionFrame>(
        networkID, envelope, simulationResult);
    return res;
}

TxSimTransactionFrame::TxSimTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionResult simulationResult)
    : TransactionFrame(networkID, envelope), mSimulationResult(simulationResult)
{
}

std::shared_ptr<OperationFrame>
TxSimTransactionFrame::makeOperation(Operation const& op, OperationResult& res,
                                     size_t index)
{
    if (mEnvelope.v0().tx.operations[index].body.type() != ACCOUNT_MERGE)
    {
        return OperationFrame::makeHelper(op, res, *this);
    }
    else
    {
        return std::make_shared<TxSimMergeOpFrame>(
            op, res, *this, mSimulationResult.result.results()[index]);
    }
}

bool
TxSimTransactionFrame::isTooEarly(LedgerTxnHeader const& header) const
{
    return mSimulationResult.result.code() == txTOO_EARLY;
}

bool
TxSimTransactionFrame::isTooLate(LedgerTxnHeader const& header) const
{
    return mSimulationResult.result.code() == txTOO_LATE;
}

bool
TxSimTransactionFrame::isBadSeq(int64_t seqNum) const
{
    return mSimulationResult.result.code() == txBAD_SEQ;
}

int64_t
TxSimTransactionFrame::getFee(LedgerHeader const& header, int64_t baseFee) const
{
    return mSimulationResult.feeCharged;
}

void
TxSimTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee)
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
        acc.seqNum = mEnvelope.v0().tx.seqNum;
    }
}

void
TxSimTransactionFrame::processSeqNum(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    if (header.current().ledgerVersion >= 10)
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        sourceAccount.current().data.account().seqNum =
            mEnvelope.v0().tx.seqNum;
    }
}
}
