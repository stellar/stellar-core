#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;

class SetOptionsOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;
    SetOptionsResult&
    innerResult()
    {
        return mResult.tr().setOptionsResult();
    }
    SetOptionsOp const& mSetOptions;

    bool addOrChangeSigner(AbstractLedgerTxn& ltx);
    void deleteSigner(AbstractLedgerTxn& ltx, LedgerTxnEntry& sourceAccount);

  public:
    SetOptionsOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    static SetOptionsResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().setOptionsResult().code();
    }
};
}
