// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/CreatePassiveOfferOpFrame.h"

namespace stellar
{

// change from CreatePassiveOfferOp to ManageOfferOp
ManageOfferOpHolder::ManageOfferOpHolder(Operation const& op)
{
    mCreateOp.body.type(MANAGE_OFFER);
    auto& manageOffer = mCreateOp.body.manageOfferOp();
    auto const& createPassiveOp = op.body.createPassiveOfferOp();
    manageOffer.amount = createPassiveOp.amount;
    manageOffer.buying = createPassiveOp.buying;
    manageOffer.selling = createPassiveOp.selling;
    manageOffer.offerID = 0;
    manageOffer.price = createPassiveOp.price;
    mCreateOp.sourceAccount = op.sourceAccount;
}

CreatePassiveOfferOpFrame::CreatePassiveOfferOpFrame(Operation const& op,
                                                     OperationResult& res,
                                                     TransactionFrame& parentTx)
    : ManageOfferOpHolder(op), ManageOfferOpFrame(mCreateOp, res, parentTx)
{
    mPassive = true;
}
}
