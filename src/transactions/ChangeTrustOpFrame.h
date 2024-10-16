#pragma once

#include "transactions/OperationFrame.h"

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{
class ChangeTrustOpFrame : public OperationFrame
{
    ChangeTrustResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().changeTrustResult();
    }
    ChangeTrustOp const& mChangeTrust;

    bool tryIncrementPoolUseCount(AbstractLedgerTxn& ltx, Asset const& asset,
                                  OperationResult& res) const;

    bool tryManagePoolOnNewTrustLine(AbstractLedgerTxn& ltx,
                                     TrustLineAsset const& tlAsset,
                                     OperationResult& res) const;

    void managePoolOnDeletedTrustLine(AbstractLedgerTxn& ltx,
                                      TrustLineAsset const& tlAsset) const;

  public:
    ChangeTrustOpFrame(Operation const& op, TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static ChangeTrustResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().changeTrustResult().code();
    }
};
}
