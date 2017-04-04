#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AccountFrame.h"
#include "ledger/LedgerManager.h"
#include "overlay/StellarXDR.h"
#include "util/types.h"
#include <memory>

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{
class Application;
class LedgerManager;
class LedgerDelta;

class SignatureChecker;
class TransactionFrame;

enum class ThresholdLevel
{
    LOW,
    MEDIUM,
    HIGH
};

class OperationFrame
{
  protected:
    Operation const& mOperation;
    TransactionFrame& mParentTx;

    AccountFrame::pointer mSourceAccount;

    bool checkSignature(SignatureChecker& signatureChecker) const;

    virtual OperationResult doCheckValid(Application& app) = 0;
    virtual OperationResult doApply(Application& app, LedgerDelta& delta,
                         LedgerManager& ledgerManager) = 0;
    virtual ThresholdLevel getThresholdLevel() const;

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, TransactionFrame& parentTx);

    OperationFrame(Operation const& op, TransactionFrame& parentTx);
    OperationFrame(OperationFrame const&) = delete;

    AccountFrame&
    getSourceAccount() const
    {
        assert(mSourceAccount);
        return *mSourceAccount;
    }

    // overrides internal sourceAccount used by this operation
    // normally set automatically by checkValid
    void
    setSourceAccountPtr(AccountFrame::pointer sa)
    {
        mSourceAccount = sa;
    }

    AccountID const& getSourceID() const;

    // load account if needed
    // returns true on success
    bool loadAccount(int ledgerProtocolVersion, LedgerDelta* delta, Database& db);

    OperationResult checkValid(SignatureChecker& signatureChecker, Application& app,
                    LedgerDelta* delta = nullptr);

    OperationResult apply(SignatureChecker& signatureChecker, LedgerDelta& delta,
               Application& app);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }
};

bool
isSuccess(OperationResult const& result);

}
