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
    innerResult(OperationResult& res) const
    {
        return res.tr().accountMergeResult();
    }

    bool doApplyBeforeV16(AbstractLedgerTxn& ltx, OperationResult& res) const;
    bool doApplyFromV16(AbstractLedgerTxn& ltx, OperationResult& res) const;

    ThresholdLevel getThresholdLevel() const override;

    virtual bool isSeqnumTooFar(AbstractLedgerTxn& ltx,
                                LedgerTxnHeader const& header,
                                AccountEntry const& sourceAccount) const;

  public:
    MergeOpFrame(Operation const& op, TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static AccountMergeResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().accountMergeResult().code();
    }
};
}
