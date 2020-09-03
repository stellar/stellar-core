// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/simulation/TxSimTransactionFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "transactions/simulation/TxSimCreateClaimableBalanceOpFrame.h"
#include "transactions/simulation/TxSimCreatePassiveSellOfferOpFrame.h"
#include "transactions/simulation/TxSimManageBuyOfferOpFrame.h"
#include "transactions/simulation/TxSimManageSellOfferOpFrame.h"
#include "transactions/simulation/TxSimMergeOpFrame.h"

namespace stellar
{
namespace txsimulation
{

TxSimTransactionFrame::TxSimTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionResult simulationResult, uint32_t partition)
    : TransactionFrame(networkID, envelope)
    , mSimulationResult(simulationResult)
    , mCount(partition)
{
}

std::shared_ptr<OperationFrame>
TxSimTransactionFrame::makeOperation(Operation const& op, OperationResult& res,
                                     size_t index)
{
    auto& ops = txbridge::getOperations(mEnvelope);
    assert(index < ops.size());
    OperationResult resultFromArchive;
    if (mSimulationResult.result.code() == txSUCCESS ||
        mSimulationResult.result.code() == txFAILED)
    {
        resultFromArchive = mSimulationResult.result.results()[index];
    }

    switch (ops[index].body.type())
    {
    case ACCOUNT_MERGE:
        return std::make_shared<TxSimMergeOpFrame>(op, res, *this,
                                                   resultFromArchive);
    case MANAGE_BUY_OFFER:
        return std::make_shared<TxSimManageBuyOfferOpFrame>(
            op, res, *this, resultFromArchive, mCount);
    case MANAGE_SELL_OFFER:
        return std::make_shared<TxSimManageSellOfferOpFrame>(
            op, res, *this, resultFromArchive, mCount);
    case CREATE_PASSIVE_SELL_OFFER:
        return std::make_shared<TxSimCreatePassiveSellOfferOpFrame>(
            op, res, *this, resultFromArchive, mCount);
    case CREATE_CLAIMABLE_BALANCE:
        return std::make_shared<TxSimCreateClaimableBalanceOpFrame>(
            op, res, *this, static_cast<uint32_t>(index), resultFromArchive,
            mCount);
    default:
        return OperationFrame::makeHelper(op, res, *this,
                                          static_cast<uint32_t>(index));
    }
}

bool
TxSimTransactionFrame::isTooEarly(LedgerTxnHeader const& header,
                                  uint64_t lowerBoundCloseTimeOffset) const
{
    return mSimulationResult.result.code() == txTOO_EARLY;
}

bool
TxSimTransactionFrame::isTooLate(LedgerTxnHeader const& header,
                                 uint64_t upperBoundCloseTimeOffset) const
{
    return mSimulationResult.result.code() == txTOO_LATE;
}

bool
TxSimTransactionFrame::isBadSeq(int64_t seqNum) const
{
    return mSimulationResult.result.code() == txBAD_SEQ;
}

int64_t
TxSimTransactionFrame::getFee(LedgerHeader const& header, int64_t baseFee,
                              bool applying) const
{
    return mSimulationResult.feeCharged;
}

void
TxSimTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee)
{
    mCachedAccount.reset();

    auto header = ltx.loadHeader();
    resetResults(header.current(), baseFee, true);

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
        acc.seqNum = getSeqNum();
    }
}

void
TxSimTransactionFrame::processSeqNum(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    if (header.current().ledgerVersion >= 10)
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        sourceAccount.current().data.account().seqNum = getSeqNum();
    }
}
}
}
