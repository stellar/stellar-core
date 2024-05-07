#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class TrustFlagsOpFrameBase : public OperationFrame
{
  private:
    virtual void setResultSelfNotAllowed() = 0;
    virtual void setResultNoTrustLine() = 0;
    virtual void setResultLowReserve() = 0;
    virtual void setResultSuccess() = 0;
    virtual bool
    isAuthRevocationValid(AbstractLedgerTxn& ltx, bool& authRevocable,
                          MutableTransactionResultBase& txResult) = 0;
    virtual bool isRevocationToMaintainLiabilitiesValid(
        bool authRevocable, LedgerTxnEntry const& trust, uint32_t flags) = 0;

    virtual AccountID const& getOpTrustor() const = 0;
    virtual Asset const& getOpAsset() const = 0;
    virtual uint32_t getOpIndex() const = 0;

    virtual bool calcExpectedFlagValue(LedgerTxnEntry const& trust,
                                       uint32_t& expectedVal) = 0;
    virtual void setFlagValue(AbstractLedgerTxn& ltx, LedgerKey const& key,
                              uint32_t flagVal) = 0;

    bool removeOffers(AbstractLedgerTxn& ltx);
    ThresholdLevel getThresholdLevel() const override;

  public:
    TrustFlagsOpFrameBase(Operation const& op, OperationResult& res,
                          TransactionFrame const& parentTx);

    bool doApply(AbstractLedgerTxn& ltx,
                 MutableTransactionResultBase& txResult) override;
};

}