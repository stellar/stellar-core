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
    virtual Hash getBalanceID() const;

    CreateClaimableBalanceResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().createClaimableBalanceResult();
    }
    CreateClaimableBalanceOp const& mCreateClaimableBalance;

    uint32_t mOpIndex;

  public:
    CreateClaimableBalanceOpFrame(Operation const& op,
                                  TransactionFrame const& parentTx,
                                  uint32_t index);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static CreateClaimableBalanceResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createClaimableBalanceResult().code();
    }
};
}
