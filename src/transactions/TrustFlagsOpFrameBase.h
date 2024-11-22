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
    virtual void setResultSelfNotAllowed(OperationResult& res) const = 0;
    virtual void setResultNoTrustLine(OperationResult& res) const = 0;
    virtual void setResultLowReserve(OperationResult& res) const = 0;
    virtual void setResultSuccess(OperationResult& res) const = 0;
    virtual bool isAuthRevocationValid(AbstractLedgerTxn& ltx,
                                       bool& authRevocable,
                                       OperationResult& res) const = 0;
    virtual bool isRevocationToMaintainLiabilitiesValid(
        bool authRevocable, LedgerTxnEntry const& trust, uint32_t flags,
        OperationResult& res) const = 0;

    virtual AccountID const& getOpTrustor() const = 0;
    virtual Asset const& getOpAsset() const = 0;
    virtual uint32_t getOpIndex() const = 0;

    virtual bool calcExpectedFlagValue(LedgerTxnEntry const& trust,
                                       uint32_t& expectedVal,
                                       OperationResult& res) const = 0;
    virtual void setFlagValue(AbstractLedgerTxn& ltx, LedgerKey const& key,
                              uint32_t flagVal) const = 0;

    bool removeOffers(AbstractLedgerTxn& ltx, OperationResult& res) const;
    ThresholdLevel getThresholdLevel() const override;

  public:
    TrustFlagsOpFrameBase(Operation const& op,
                          TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
};

}