// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/CreatePassiveOfferOpFrame.h"

namespace stellar
{
    CreatePassiveOfferOpFrame::CreatePassiveOfferOpFrame(Operation const& op,
        OperationResult& res,
        TransactionFrame& parentTx)
        : ManageOfferOpFrame(createMangeOfferOp(op), res, parentTx)
    {
        mCreateOp.body.type(MANAGE_OFFER);
        mCreateOp.body.manageOfferOp().amount = op.body.createPassiveOfferOp().amount;
        mCreateOp.body.manageOfferOp().takerGets = op.body.createPassiveOfferOp().takerGets;
        mCreateOp.body.manageOfferOp().takerPays = op.body.createPassiveOfferOp().takerPays;
        mCreateOp.body.manageOfferOp().offerID = 0;
        mCreateOp.body.manageOfferOp().price = op.body.createPassiveOfferOp().price;
        mCreateOp.sourceAccount = op.sourceAccount;
        mPassive = true;

    }

    // change from CreatePassiveOfferOp to ManageOfferOp
    Operation& CreatePassiveOfferOpFrame::createMangeOfferOp(Operation const& op)
    {
        mCreateOp.body.type(MANAGE_OFFER);
        mCreateOp.body.manageOfferOp().amount = op.body.createPassiveOfferOp().amount;
        mCreateOp.body.manageOfferOp().takerGets = op.body.createPassiveOfferOp().takerGets;
        mCreateOp.body.manageOfferOp().takerPays = op.body.createPassiveOfferOp().takerPays;
        mCreateOp.body.manageOfferOp().offerID = 0;
        mCreateOp.body.manageOfferOp().price = op.body.createPassiveOfferOp().price;
        mCreateOp.sourceAccount = op.sourceAccount;

        return mCreateOp;
    }

}

