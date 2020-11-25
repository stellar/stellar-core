#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OfferExchange.h"
#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;

class PathPaymentOpFrameBase : public OperationFrame
{
  protected:
    bool convert(AbstractLedgerTxn& ltx, int64_t maxOffersToCross,
                 Asset const& sendAsset, int64_t maxSend, int64_t& amountSend,
                 Asset const& recvAsset, int64_t maxRecv, int64_t& amountRecv,
                 RoundingType round, std::vector<ClaimOfferAtom>& offerTrail);

    bool shouldBypassIssuerCheck(std::vector<Asset> const& path) const;

    bool updateSourceBalance(AbstractLedgerTxn& ltx, int64_t amount,
                             bool bypassIssuerCheck,
                             bool doesSourceAccountExist);

    bool updateDestBalance(AbstractLedgerTxn& ltx, int64_t amount,
                           bool bypassIssuerCheck);

    bool checkIssuer(AbstractLedgerTxn& ltx, Asset const& asset);

  public:
    PathPaymentOpFrameBase(Operation const& op, OperationResult& res,
                           TransactionFrame& parentTx);

    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    virtual bool checkTransfer(int64_t maxSend, int64_t amountSend,
                               int64_t maxRecv, int64_t amountRecv) const = 0;

    virtual Asset const& getSourceAsset() const = 0;
    virtual Asset const& getDestAsset() const = 0;
    AccountID getDestID() const;
    virtual MuxedAccount const& getDestMuxedAccount() const = 0;
    virtual xdr::xvector<Asset, 5> const& getPath() const = 0;

    virtual void setResultSuccess() = 0;
    virtual void setResultMalformed() = 0;
    virtual void setResultUnderfunded() = 0;
    virtual void setResultSourceNoTrust() = 0;
    virtual void setResultSourceNotAuthorized() = 0;
    virtual void setResultNoDest() = 0;
    virtual void setResultDestNoTrust() = 0;
    virtual void setResultDestNotAuthorized() = 0;
    virtual void setResultLineFull() = 0;
    virtual void setResultNoIssuer(Asset const& asset) = 0;
    virtual void setResultTooFewOffers() = 0;
    virtual void setResultOfferCrossSelf() = 0;
    virtual void setResultConstraintNotMet() = 0;
};
}
