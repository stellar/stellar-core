#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class ManageOfferOpFrameBase : public OperationFrame
{
    Asset const mSheep;
    Asset const mWheat;
    int64_t const mOfferID;
    Price const mPrice;

    bool const mSetPassiveOnCreate;

    bool checkOfferValid(AbstractLedgerTxn& ltxOuter,
                         OperationResult& res) const;

    bool computeOfferExchangeParameters(AbstractLedgerTxn& ltxOuter,
                                        OperationResult& res,
                                        bool creatingNewOffer,
                                        int64_t& maxSheepSend,
                                        int64_t& maxWheatReceive) const;

    LedgerEntry buildOffer(int64_t amount, uint32_t flags,
                           LedgerEntry::_ext_t const& extension) const;

    virtual int64_t generateNewOfferID(LedgerTxnHeader& header) const;

  public:
    ManageOfferOpFrameBase(Operation const& op,
                           TransactionFrame const& parentTx, Asset const& sheep,
                           Asset const& wheat, int64_t offerID,
                           Price const& price, bool setPassiveOnCreate);

    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltxOuter,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;

    bool isDexOperation() const override;

    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    virtual bool isAmountValid() const = 0;
    virtual bool isDeleteOffer() const = 0;

    virtual int64_t getOfferBuyingLiabilities() const = 0;
    virtual int64_t getOfferSellingLiabilities() const = 0;

    virtual void applyOperationSpecificLimits(int64_t& maxSheepSend,
                                              int64_t sheepSent,
                                              int64_t& maxWheatReceive,
                                              int64_t wheatReceived) const = 0;
    virtual void
    getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                   int64_t& maxWheatReceive) const = 0;

    virtual ManageOfferSuccessResult&
    getSuccessResult(OperationResult& res) const = 0;

    virtual void setResultSuccess(OperationResult& res) const = 0;
    virtual void setResultMalformed(OperationResult& res) const = 0;
    virtual void setResultSellNoTrust(OperationResult& res) const = 0;
    virtual void setResultBuyNoTrust(OperationResult& res) const = 0;
    virtual void setResultSellNotAuthorized(OperationResult& res) const = 0;
    virtual void setResultBuyNotAuthorized(OperationResult& res) const = 0;
    virtual void setResultLineFull(OperationResult& res) const = 0;
    virtual void setResultUnderfunded(OperationResult& res) const = 0;
    virtual void setResultCrossSelf(OperationResult& res) const = 0;
    virtual void setResultSellNoIssuer(OperationResult& res) const = 0;
    virtual void setResultBuyNoIssuer(OperationResult& res) const = 0;
    virtual void setResultNotFound(OperationResult& res) const = 0;
    virtual void setResultLowReserve(OperationResult& res) const = 0;
};
}
