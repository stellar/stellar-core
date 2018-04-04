#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "util/types.h"
#include <memory>

#include "ledger/AccountReference.h"

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{
class Application;
class LedgerState;

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
    OperationResult& mResult;

    bool checkSignature(SignatureChecker& signatureChecker,
                        AccountReference account) const;

    virtual bool doCheckValid(Application& app, uint32_t ledgerVersion) = 0;
    virtual bool doApply(Application& app, LedgerState& ls) = 0;
    // returns the threshold this operation requires
    virtual ThresholdLevel getThresholdLevel() const;

    AccountReference loadSourceAccount(LedgerState& ls);
    AccountReference loadSourceAccount(
            LedgerState& ls, std::shared_ptr<LedgerHeaderReference> header);

    // returns true if the operation is supported given a protocol version
    virtual bool isVersionSupported(uint32_t protocolVersion) const;

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, OperationResult& res,
               TransactionFrame& parentTx);

    OperationFrame(Operation const& op, OperationResult& res,
                   TransactionFrame& parentTx);
    OperationFrame(OperationFrame const&) = delete;

    AccountID const& getSourceID() const;

    OperationResult&
    getResult() const
    {
        return mResult;
    }
    OperationResultCode getResultCode() const;

    bool checkValid(SignatureChecker& signatureChecker, Application& app,
                    LedgerState& ls, bool forApply);

    bool apply(SignatureChecker& signatureChecker, LedgerState& ls,
               Application& app);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }
};
}
