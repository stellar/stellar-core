// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerRange.h"

#include <cassert>
#include <fmt/format.h>

namespace stellar
{

LedgerRange::LedgerRange(uint32_t first, uint32_t count)
    : mFirst{first}, mCount{count}
{
    assert(count == 0 || mFirst > 0);
}

std::string
LedgerRange::toString() const
{
    return fmt::format("[{},{})", mFirst, mFirst + mCount);
}

bool
operator==(LedgerRange const& x, LedgerRange const& y)
{
    if (x.mFirst != y.mFirst)
    {
        return false;
    }
    if (x.mCount != y.mCount)
    {
        return false;
    }
    return true;
}

bool
operator!=(LedgerRange const& x, LedgerRange const& y)
{
    return !(x == y);
}
}
