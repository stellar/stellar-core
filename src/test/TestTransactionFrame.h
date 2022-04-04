#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{

class TestTransactionFrame : public TransactionFrame
{
  private:
    TransactionResult mResult;
    void
    syncResult(TransactionResult& txResult)
    {
        if (&txResult != &mResult)
        {
            txResult = mResult;
        }
    }

  public:
    TestTransactionFrame(Hash const& networkID,
                         TransactionEnvelope const& envelope)
        : TransactionFrame(networkID, envelope)
    {
    }

    virtual ~TestTransactionFrame() = default;

    TransactionResult&
    getResult() override
    {
        return mResult;
    }

    TransactionResultCode
    getResultCode() const override
    {
        return mResult.result.code();
    }

    void resetResults(LedgerHeader const& header, int64_t baseFee,
                      bool applying, TransactionResult& txResult) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset,
                    TransactionResult& txResult) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset,
                    TransactionResult& txResult) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) override;
    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee,
                          TransactionResult& txResult) override;

    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;

    bool apply(Application& app, AbstractLedgerTxn& ltx, TransactionMeta& meta,
               bool chargeFee, TransactionResult& txResult) override;

    bool apply(Application& app, AbstractLedgerTxn& ltx, TransactionMeta& meta,
               TransactionResult& txResult) override;

    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMeta& meta) override;

    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionResult& txResult) override;
};

class TestFeeBumpTransactionFrame : public FeeBumpTransactionFrame
{
  private:
    TransactionResult mResult;

    void
    syncResult(TransactionResult& txResult)
    {
        if (&txResult != &mResult)
        {
            txResult = mResult;
        }
    }

  public:
    TestFeeBumpTransactionFrame(Hash const& networkID,
                                TransactionEnvelope const& envelope,
                                TransactionFramePtr innerTx)
        : FeeBumpTransactionFrame(networkID, envelope, innerTx)
    {
    }

    virtual ~TestFeeBumpTransactionFrame() = default;

    TransactionResult&
    getResult() override
    {
        return mResult;
    }

    TransactionResultCode
    getResultCode() const override
    {
        return mResult.result.code();
    }

    bool apply(Application& app, AbstractLedgerTxn& ltx, TransactionMeta& meta,
               TransactionResult& txResult) override;

    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMeta& meta) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset,
                    TransactionResult& txResult) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) override;
    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee,
                          TransactionResult& txResult) override;

    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;
};

}