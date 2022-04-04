// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrameBase.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{

TransactionFrameBasePtr
TransactionFrameBase::makeTransactionFromWire(
    Hash const& networkID, TransactionEnvelope const& env,
    std::optional<bool> isDiscounted)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
    case ENVELOPE_TYPE_TX:
        return std::make_shared<TransactionFrame>(networkID, env, isDiscounted);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return std::make_shared<FeeBumpTransactionFrame>(networkID, env,
                                                         isDiscounted);
    default:
        abort();
    }
}
}
