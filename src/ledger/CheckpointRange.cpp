// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/CheckpointRange.h"
#include <cassert>

namespace stellar
{

CheckpointRange::CheckpointRange(uint32_t first, uint32_t last)
    : mFirst{first}, mLast{last}
{
    assert(mFirst > 0);
    assert(mLast >= mFirst);
}

bool operator == (CheckpointRange const& x, CheckpointRange const& y)
{
    if (x.mFirst != y.mFirst)
    {
        return false;
    }
    if (x.mLast != y.mLast)
    {
        return false;
    }
    return true;
}

bool operator != (CheckpointRange const& x, CheckpointRange const& y)
{
    return !(x == y);
}

}
