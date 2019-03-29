#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ManageSellOfferOpFrame.h"

namespace stellar
{
class ManageSellOfferOpHolder
{
  public:
    ManageSellOfferOpHolder(Operation const& op);
    Operation mCreateOp;
};

class CreatePassiveSellOfferOpFrame : public ManageSellOfferOpHolder,
                                      public ManageSellOfferOpFrame
{
  public:
    CreatePassiveSellOfferOpFrame(Operation const& op, OperationResult& res,
                                  TransactionFrame& parentTx);
};
}
