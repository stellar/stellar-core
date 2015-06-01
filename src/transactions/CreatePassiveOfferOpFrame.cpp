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
        mPassive = true;

    }

    // change from CreatePassiveOfferOp to ManageOfferOp
    Operation& CreatePassiveOfferOpFrame::createMangeOfferOp(Operation const& op)
    {
        return mCreateOp;
    }

}

