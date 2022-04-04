// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestTransactionFrame.h"
#include "transactions/TransactionFrameBase.h"

namespace stellar
{

void
TestTransactionFrame::resetResults(LedgerHeader const& header, int64_t baseFee,
                                   bool applying, TransactionResult& txResult)
{
    TransactionFrame::resetResults(header, baseFee, applying, mResult);
    syncResult(txResult);
}

bool
TestTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                 SequenceNumber current, bool chargeFee,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 TransactionResult& txResult)
{
    bool res = TransactionFrame::checkValid(ltxOuter, current, chargeFee,
                                            lowerBoundCloseTimeOffset,
                                            upperBoundCloseTimeOffset, mResult);
    syncResult(txResult);
    return res;
}

bool
TestTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 TransactionResult& txResult)
{
    bool res = TransactionFrame::checkValid(ltxOuter, current,
                                            lowerBoundCloseTimeOffset,
                                            upperBoundCloseTimeOffset, mResult);
    syncResult(txResult);
    return res;
}

bool
TestTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                 SequenceNumber current,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset)
{
    return checkValid(ltxOuter, current, lowerBoundCloseTimeOffset,
                      upperBoundCloseTimeOffset, mResult);
}

void
TestTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee,
                                       TransactionResult& txResult)
{
    TransactionFrame::processFeeSeqNum(ltx, baseFee, mResult);
    syncResult(txResult);
}

void
TestTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee)
{
    processFeeSeqNum(ltx, baseFee, mResult);
}

bool
TestTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                            TransactionMeta& meta, bool chargeFee,
                            TransactionResult& txResult)
{
    bool res = TransactionFrame::apply(app, ltx, meta, chargeFee, mResult);
    syncResult(txResult);
    return res;
}

bool
TestTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                            TransactionMeta& meta, TransactionResult& txResult)
{
    bool res = TransactionFrame::apply(app, ltx, meta, mResult);
    syncResult(txResult);
    return res;
}

bool
TestTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                            TransactionMeta& meta)
{
    return apply(app, ltx, meta, mResult);
}

bool
TestTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                            TransactionResult& txResult)
{
    bool res = TransactionFrame::apply(app, ltx, mResult);
    syncResult(txResult);
    return res;
}

bool
TestFeeBumpTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                                   TransactionMeta& meta,
                                   TransactionResult& txResult)
{
    bool res = FeeBumpTransactionFrame::apply(app, ltx, meta, mResult);
    syncResult(txResult);
    return res;
}

bool
TestFeeBumpTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                                   TransactionMeta& meta)
{
    return apply(app, ltx, meta, mResult);
}

bool
TestFeeBumpTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                        SequenceNumber current,
                                        uint64_t lowerBoundCloseTimeOffset,
                                        uint64_t upperBoundCloseTimeOffset,
                                        TransactionResult& txResult)
{
    bool res = FeeBumpTransactionFrame::checkValid(
        ltxOuter, current, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
        mResult);
    syncResult(txResult);
    return res;
}

bool
TestFeeBumpTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                        SequenceNumber current,
                                        uint64_t lowerBoundCloseTimeOffset,
                                        uint64_t upperBoundCloseTimeOffset)
{
    return checkValid(ltxOuter, current, lowerBoundCloseTimeOffset,
                      upperBoundCloseTimeOffset, mResult);
}

void
TestFeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                              int64_t baseFee,
                                              TransactionResult& txResult)
{
    FeeBumpTransactionFrame::processFeeSeqNum(ltx, baseFee, mResult);
    syncResult(txResult);
}

void
TestFeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                              int64_t baseFee)
{
    processFeeSeqNum(ltx, baseFee, mResult);
}

TransactionFrameBasePtr
TransactionFrameBase::makeTestTransactionFromWire(
    Hash const& networkID, TransactionEnvelope const& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
    case ENVELOPE_TYPE_TX:
        return std::make_shared<TestTransactionFrame>(networkID, env);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return std::make_shared<TestFeeBumpTransactionFrame>(
            networkID, env,
            std::make_shared<TestTransactionFrame>(
                networkID, FeeBumpTransactionFrame::convertInnerTxToV1(env)));
    default:
        abort();
    }
}
}