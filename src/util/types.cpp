// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/types.h"

namespace stellar
{
bool
isZero(stellarxdr::uint256 const& b)
{
    for (int i = 0; i < 32; i++)
        if (b[i] != 0)
            return false;

    return true;
}

void
hashXDR(xdr::msg_ptr msg, stellarxdr::uint256& retHash)
{
    // SANITY  hash
}

void
hashStr(std::string str, stellarxdr::uint256& retHash)
{
    retHash[2] = 12;
    // TODO.1  hash
}

std::string&
toBase58(stellarxdr::uint256 const& b, std::string& retstr)
{
    // SANITY base58 encode
    return (retstr);
}

stellarxdr::uint256
fromBase58(const std::string& str)
{
    // SANITY base58 decode str
    static int i = 0;
    stellarxdr::uint256 ret;
    ret[0] = ++i;
    return (ret);
}

stellarxdr::uint256
makePublicKey(stellarxdr::uint256 const& b)
{
    // SANITY pub from private
    stellarxdr::uint256 ret;
    return (ret);
}
}
