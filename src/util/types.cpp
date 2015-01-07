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

bool compareCurrency(Currency& first, Currency& second)
{
    if(first.type() != second.type()) return false;

    if(first.type()==NATIVE)
    {
        if(second.type() == NATIVE) return true;
    } else if(second.type() == ISO4217)
    {
        if((first.isoCI().issuer == second.isoCI().issuer) &&
            (first.isoCI().currencyCode == second.isoCI().currencyCode)) return true;

    }
    return false;
}

void currencyCodeToStr(xdr::opaque_array<4U>& code, std::string& retStr)
{
    retStr = "    ";
    for(int n = 0; n < 4; n++)
    {
        retStr[n] = code[n];
    }
}

void strToCurrencyCode(xdr::opaque_array<4U>& ret, const std::string& str)
{
    for(int n = 0; (n < str.size()) && (n < 4); n++)
    {
        ret[n] = str[n];
    }
}

}
