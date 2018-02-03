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
    OperationResult& mResult;

    bool checkSignature(SignatureChecker& signatureChecker) const;

    virtual bool doCheckValid(Application& app) = 0;
    virtual bool doApply(Application& app, LedgerDelta& delta,
                         LedgerManager& ledgerManager) = 0;
    // returns the threshold this operation requires
    virtual ThresholdLevel getThresholdLevel() const;

    // returns true if the operation is supported given a protocol version
    virtual bool isVersionSupported(uint32_t protocolVersion) const;

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
    bool loadAccount(int ledgerProtocolVersion, LedgerDelta* delta,
                     Database& db);

    OperationResult&
    getResult() const
    {
        return mResult;
    }
    OperationResultCode getResultCode() const;

    bool checkValid(SignatureChecker& signatureChecker, Application& app,
                    LedgerDelta* delta = nullptr);

    bool apply(SignatureChecker& signatureChecker, LedgerDelta& delta,
               Application& app);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }
};
}
