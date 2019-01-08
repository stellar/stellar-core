#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;

class ManageDataOpFrame : public OperationFrame
{

    ManageDataResult&
    innerResult()
    {
        return mResult.tr().manageDataResult();
    }

    ManageDataOp const& mManageData;

  public:
    ManageDataOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(Application& app, AbstractLedgerTxn& ltx) override;
    bool doCheckValid(Application& app, uint32_t ledgerVersion) override;

    static ManageDataResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageDataResult().code();
    }
};
}
