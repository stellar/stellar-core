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
    innerResult(OperationResult& res) const
    {
        return res.tr().pathPaymentStrictReceiveResult();
    }

  public:
    PathPaymentStrictReceiveOpFrame(Operation const& op,
                                    TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    bool checkTransfer(int64_t maxSend, int64_t amountSend, int64_t maxRecv,
                       int64_t amountRecv) const override;

    Asset const& getSourceAsset() const override;
    Asset const& getDestAsset() const override;
    MuxedAccount const& getDestMuxedAccount() const override;
    xdr::xvector<Asset, 5> const& getPath() const override;

    void setResultSuccess(OperationResult& res) const override;
    void setResultMalformed(OperationResult& res) const override;
    void setResultUnderfunded(OperationResult& res) const override;
    void setResultSourceNoTrust(OperationResult& res) const override;
    void setResultSourceNotAuthorized(OperationResult& res) const override;
    void setResultNoDest(OperationResult& res) const override;
    void setResultDestNoTrust(OperationResult& res) const override;
    void setResultDestNotAuthorized(OperationResult& res) const override;
    void setResultLineFull(OperationResult& res) const override;
    void setResultNoIssuer(Asset const& asset,
                           OperationResult& res) const override;
    void setResultTooFewOffers(OperationResult& res) const override;
    void setResultOfferCrossSelf(OperationResult& res) const override;
    void setResultConstraintNotMet(OperationResult& res) const override;

    static PathPaymentStrictReceiveResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().pathPaymentStrictReceiveResult().code();
    }
};
}
