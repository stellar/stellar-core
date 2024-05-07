#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class CreateClaimableBalanceOpFrame : public OperationFrame
{
    virtual Hash getBalanceID();

    CreateClaimableBalanceResult&
    innerResult()
    {
        return mResult.tr().createClaimableBalanceResult();
    }
    CreateClaimableBalanceOp const& mCreateClaimableBalance;

    uint32_t mOpIndex;

  public:
    CreateClaimableBalanceOpFrame(Operation const& op, OperationResult& res,
                                  TransactionFrame const& parentTx,
                                  uint32_t index);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AbstractLedgerTxn& ltx,
                 MutableTransactionResultBase& txResult) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static CreateClaimableBalanceResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createClaimableBalanceResult().code();
    }
};
}
