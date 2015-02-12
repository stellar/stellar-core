// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/types.h"
#include "lib/util/uint128_t.h"

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
    uint256 ret;
    ret[0] = b[0];
    ret[1] = b[1];
    ret[2] = b[2];
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

void currencyCodeToStr(const xdr::opaque_array<4U>& code, std::string& retStr)
{
    retStr = "    ";
    for(int n = 0; n < 4; n++)
    {
        if(code[n])
            retStr[n] = code[n];
        else
        {
            retStr.resize(n);
            return;
        }
    }
}

void strToCurrencyCode(xdr::opaque_array<4U>& ret, const std::string& str)
{
    for(int n = 0; (n < str.size()) && (n < 4); n++)
    {
        ret[n] = str[n];
    }
}

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C)
{
    // update when moving to (signed) int128
    assert((A >= 0) && (B >= 0) && (C > 0));
    uint128_t a(A);
    uint128_t b(B);
    uint128_t c(C);
    uint128_t x = (a*b)/c;
    if (x > INT64_MAX)
    {
        throw std::overflow_error("cannot process value");
    }
    return (uint64_t)x;
}

bool iequals(const std::string& a, const std::string& b)
{
    size_t sz = a.size();
    if(b.size() != sz)
        return false;
    for(size_t i = 0; i < sz; ++i)
        if(tolower(a[i]) != tolower(b[i]))
            return false;
    return true;
}

bool operator>(Price const& a, Price const& b)
{
    uint128_t l(a.n);
    uint128_t r(a.d);
    l *= b.d;
    r *= b.n;
    return l > r;
}

bool operator==(Price const& a, Price const& b)
{
    return (a.n == b.n) && (a.d == b.d);
}

}
