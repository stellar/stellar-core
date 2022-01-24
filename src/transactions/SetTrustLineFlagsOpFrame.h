#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TrustFlagsOpFrameBase.h"

namespace stellar
{
class AbstractLedgerTxn;

class SetTrustLineFlagsOpFrame : public TrustFlagsOpFrameBase
{
    SetTrustLineFlagsResult&
    innerResult()
    {
        return mResult.tr().setTrustLineFlagsResult();
    }

    SetTrustLineFlagsOp const& mSetTrustLineFlags;

    uint32_t mOpIndex;

    void setResultSelfNotAllowed() override;
    void setResultNoTrustLine() override;
    void setResultLowReserve() override;
    void setResultSuccess() override;

    bool isAuthRevocationValid(AbstractLedgerTxn& ltx,
                               bool& authRevocable) override;
    bool isRevocationToMaintainLiabilitiesValid(bool authRevocable,
                                                LedgerTxnEntry const& trust,
                                                uint32_t flags) override;

    AccountID const& getOpTrustor() const override;
    Asset const& getOpAsset() const override;
    uint32_t getOpIndex() const override;

    bool calcExpectedFlagValue(LedgerTxnEntry const& trust,
                               uint32_t& expectedVal) override;

    void setFlagValue(AbstractLedgerTxn& ltx, LedgerKey const& key,
                      uint32_t flagVal) override;

  public:
    SetTrustLineFlagsOpFrame(Operation const& op, OperationResult& res,
                             TransactionFrame& parentTx, uint32_t index);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static SetTrustLineFlagsResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().setTrustLineFlagsResult().code();
    }
};
}
