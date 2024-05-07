#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class LedgerTxnHeader;

class MergeOpFrame : public OperationFrame
{
    AccountMergeResult&
    innerResult()
    {
        return mResult.tr().accountMergeResult();
    }

    bool doApplyBeforeV16(AbstractLedgerTxn& ltx,
                          MutableTransactionResultBase& txResult);
    bool doApplyFromV16(AbstractLedgerTxn& ltx,
                        MutableTransactionResultBase& txResult);

    ThresholdLevel getThresholdLevel() const override;

    virtual bool isSeqnumTooFar(AbstractLedgerTxn& ltx,
                                LedgerTxnHeader const& header,
                                AccountEntry const& sourceAccount);

  public:
    MergeOpFrame(Operation const& op, OperationResult& res,
                 TransactionFrame const& parentTx);

    bool doApply(AbstractLedgerTxn& ltx,
                 MutableTransactionResultBase& txResult) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    static AccountMergeResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().accountMergeResult().code();
    }
};
}
