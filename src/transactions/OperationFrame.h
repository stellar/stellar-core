#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include "ledger/LedgerManager.h"
#include "ledger/AccountFrame.h"
#include "generated/StellarXDR.h"
#include "util/types.h"

namespace stellar
{
class Application;
class LedgerManager;
class LedgerDelta;

class TransactionFrame;

class OperationFrame
{
  protected:
    Operation const& mOperation;
    TransactionFrame& mParentTx;
    AccountFrame::pointer mSourceAccount;
    OperationResult& mResult;

    bool checkSignature() const;

    virtual bool doCheckValid(Application& app) = 0;
    virtual bool doApply(LedgerDelta& delta, LedgerManager& ledgerManager) = 0;
    virtual int32_t getNeededThreshold() const;

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, OperationResult& res,
               TransactionFrame& parentTx);

    OperationFrame(Operation const& op, OperationResult& res,
                   TransactionFrame& parentTx);
    OperationFrame(OperationFrame const&) = delete;

    AccountFrame&
    getSourceAccount() const
    {
        assert(mSourceAccount);
        return *mSourceAccount;
    }

    AccountID const& getSourceID() const;

    // load account if needed
    // returns true on success
    bool loadAccount(Application& app);

    OperationResult&
    getResult() const
    {
        return mResult;
    }
    OperationResultCode getResultCode() const;

    bool checkValid(Application& app);

    bool apply(LedgerDelta& delta, Application& app);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }
};
}
