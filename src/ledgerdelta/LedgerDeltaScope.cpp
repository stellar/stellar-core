// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledgerdelta/LedgerDeltaScope.h"
#include "ledgerdelta/LedgerDelta.h"

namespace stellar
{

LedgerDeltaScope::LedgerDeltaScope(LedgerDelta& stack) : mStack{stack}
{
    mStack.newDelta();
}

LedgerDeltaScope::~LedgerDeltaScope()
{
    if (!mCommited)
    {
        mStack.rollbackTop();
    }
}

void LedgerDeltaScope::commit()
{
    if (mCommited)
    {
        throw std::runtime_error(
            "Invalid operation: delta is already committed");
    }

    mCommited = true;
    mStack.applyTop();
}

}
