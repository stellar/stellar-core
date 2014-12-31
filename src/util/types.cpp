// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/types.h"

namespace stellar
{
bool
isZero(uint256 const& b)
{
    for (auto i : b)
        if (i != 0)
            return false;

    return true;
}

uint256
makePublicKey(uint256 const& b)
{
    // SANITY pub from private
    static int i = 0;
    uint256 ret;
    ret[0] = ++i;
    return (ret);
}
}
