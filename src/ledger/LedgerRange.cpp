// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerRange.h"
#include "util/GlobalChecks.h"

#include <fmt/format.h>
#include <limits>
#include <stdexcept>

namespace stellar
{

LedgerRange::LedgerRange(uint32_t first, uint32_t count)
    : mFirst{first}, mCount{count}
{
    releaseAssert(count == 0 || mFirst > 0);
}

LedgerRange
LedgerRange::inclusive(uint32_t first, uint32_t last)
{
    // LedgerRange is half-open: in exchange for being able to represent
    // empty ranges, it can't represent ranges that include UINT32_MAX.
    releaseAssert(last < std::numeric_limits<uint32_t>::max());
    return LedgerRange(first, last - first + 1);
}

uint32_t
LedgerRange::last() const
{
    if (mCount == 0)
    {
        throw std::logic_error("last() cannot be called on "
                               "LedgerRange when mCount == 0");
    }
    return limit() - 1;
}

std::string
LedgerRange::toString() const
{
    return fmt::format(FMT_STRING("[{:d},{:d})"), mFirst, mFirst + mCount);
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
