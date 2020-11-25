#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class CreateAccountOpFrame : public OperationFrame
{
    CreateAccountResult&
    innerResult()
    {
        return mResult.tr().createAccountResult();
    }
    CreateAccountOp const& mCreateAccount;

    bool doApplyBeforeV14(AbstractLedgerTxn& ltx);
    bool doApplyFromV14(AbstractLedgerTxn& ltxOuter);

    bool checkLowReserve(AbstractLedgerTxn& ltx);
    bool deductStartingBalance(AbstractLedgerTxn& ltx);
    void createAccount(AbstractLedgerTxn& ltx);

  public:
    CreateAccountOpFrame(Operation const& op, OperationResult& res,
                         TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static CreateAccountResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createAccountResult().code();
    }
};
}
