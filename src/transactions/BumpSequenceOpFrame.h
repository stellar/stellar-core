#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "transactions/OperationFrame.h"

namespace stellar
{
class BumpSequenceOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;
    bool isOpSupported(LedgerHeader const& header) const override;

    BumpSequenceResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().bumpSeqResult();
    }
    BumpSequenceOp const& mBumpSequenceOp;

  public:
    BumpSequenceOpFrame(Operation const& op, TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static BumpSequenceResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().bumpSeqResult().code();
    }
};
}
