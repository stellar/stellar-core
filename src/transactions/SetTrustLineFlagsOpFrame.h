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
    innerResult(OperationResult& res) const
    {
        return res.tr().setTrustLineFlagsResult();
    }

    SetTrustLineFlagsOp const& mSetTrustLineFlags;

    uint32_t mOpIndex;

    void setResultSelfNotAllowed(OperationResult& res) const override;
    void setResultNoTrustLine(OperationResult& res) const override;
    void setResultLowReserve(OperationResult& res) const override;
    void setResultSuccess(OperationResult& res) const override;

    bool isAuthRevocationValid(AbstractLedgerTxn& ltx, bool& authRevocable,
                               OperationResult& res) const override;
    bool isRevocationToMaintainLiabilitiesValid(
        bool authRevocable, LedgerTxnEntry const& trust, uint32_t flags,
        OperationResult& res) const override;

    AccountID const& getOpTrustor() const override;
    Asset const& getOpAsset() const override;
    uint32_t getOpIndex() const override;

    bool calcExpectedFlagValue(LedgerTxnEntry const& trust,
                               uint32_t& expectedVal,
                               OperationResult& res) const override;

    void setFlagValue(AbstractLedgerTxn& ltx, LedgerKey const& key,
                      uint32_t flagVal) const override;

  public:
    SetTrustLineFlagsOpFrame(Operation const& op,
                             TransactionFrame const& parentTx, uint32_t index);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static SetTrustLineFlagsResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().setTrustLineFlagsResult().code();
    }
};
}
