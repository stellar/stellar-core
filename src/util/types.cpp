// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/types.h"

namespace stellar
{
bool
isZero(uint256 const& b)
{
    for (int i = 0; i < 32; i++)
        if (b[i] != 0)
            return false;

    return true;
}

void
hashXDR(xdr::msg_ptr msg, uint256& retHash)
{
    // SANITY  hash
}

void
hashStr(std::string str, uint256& retHash)
{
    retHash[2] = 12;
    // TODO.1  hash
}


std::string&
toBase58(uint256 const& b, std::string& retstr)
{
// SANITY base58 encode
return (retstr);
}

std::string&
toBase58(uint160 const& b, std::string& retstr)
{
    // SANITY base58 encode
    return (retstr);
}


uint256
fromBase58(const std::string& str)
{
    // SANITY base58 decode str
    static int i = 0;
    uint256 ret;
    ret[0] = ++i;
    return (ret);
}

uint160 base58to160(const std::string& str)
{
    // TODO.2 
    static int i = 0;
    uint160 ret;
    ret[0] = ++i;
    return (ret);
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
