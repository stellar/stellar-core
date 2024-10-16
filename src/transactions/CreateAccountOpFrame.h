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
    innerResult(OperationResult& res) const
    {
        return res.tr().createAccountResult();
    }
    CreateAccountOp const& mCreateAccount;

    bool doApplyBeforeV14(AbstractLedgerTxn& ltx, OperationResult& res) const;
    bool doApplyFromV14(AbstractLedgerTxn& ltxOuter,
                        OperationResult& res) const;

  public:
    CreateAccountOpFrame(Operation const& op, TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static CreateAccountResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createAccountResult().code();
    }
};
}
