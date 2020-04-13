// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/PathPaymentOpFrameBase.h"

namespace stellar
{

class PathPaymentStrictReceiveOpFrame : public PathPaymentOpFrameBase
{
    PathPaymentStrictReceiveOp const& mPathPayment;

    PathPaymentStrictReceiveResult&
    innerResult()
    {
        return mResult.tr().pathPaymentStrictReceiveResult();
    }

  public:
    PathPaymentStrictReceiveOpFrame(Operation const& op, OperationResult& res,
                                    TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    bool checkTransfer(int64_t maxSend, int64_t amountSend, int64_t maxRecv,
                       int64_t amountRecv) const override;

    Asset const& getSourceAsset() const override;
    Asset const& getDestAsset() const override;
    MuxedAccount const& getDestMuxedAccount() const override;
    xdr::xvector<Asset, 5> const& getPath() const override;

    void setResultSuccess() override;
    void setResultMalformed() override;
    void setResultUnderfunded() override;
    void setResultSourceNoTrust() override;
    void setResultSourceNotAuthorized() override;
    void setResultNoDest() override;
    void setResultDestNoTrust() override;
    void setResultDestNotAuthorized() override;
    void setResultLineFull() override;
    void setResultNoIssuer(Asset const& asset) override;
    void setResultTooFewOffers() override;
    void setResultOfferCrossSelf() override;
    void setResultConstraintNotMet() override;

    static PathPaymentStrictReceiveResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().pathPaymentStrictReceiveResult().code();
    }
};
}
