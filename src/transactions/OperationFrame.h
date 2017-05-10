#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManager.h"
#include "overlay/StellarXDR.h"
#include "util/optional.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"

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
class LedgerEntries;
class SignatureChecker;
class TransactionFrame;
struct SigningAccount;

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

    bool checkSignature(SigningAccount const& signingAccount, SignatureChecker& signatureChecker) const;

    virtual bool doCheckValid(Application& app) = 0;
    virtual bool doApply(Application& app, LedgerDelta& ledgerDelta,
                         LedgerManager& ledgerManager) = 0;
    virtual ThresholdLevel getThresholdLevel() const;

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
                    LedgerDelta* ledgerDelta = nullptr);

    bool apply(SignatureChecker& signatureChecker, LedgerDelta& ledgerDelta,
               Application& app);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }
};
}
