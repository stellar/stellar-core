// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{

class BeginSponsoringFutureReservesOpFrame : public OperationFrame
{
    bool isOpSupported(LedgerHeader const& header) const override;

    BeginSponsoringFutureReservesResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().beginSponsoringFutureReservesResult();
    }
    BeginSponsoringFutureReservesOp const& mBeginSponsoringFutureReservesOp;

    void createSponsorship(AbstractLedgerTxn& ltx) const;
    void createSponsorshipCounter(AbstractLedgerTxn& ltx) const;

  public:
    BeginSponsoringFutureReservesOpFrame(Operation const& op,
                                         TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static BeginSponsoringFutureReservesResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().beginSponsoringFutureReservesResult().code();
    }
};
}
