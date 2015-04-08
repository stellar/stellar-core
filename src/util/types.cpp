// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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

bool
compareCurrency(Currency const& first, Currency const& second)
{
    if (first.type() != second.type())
        return false;

    if (first.type() == NATIVE)
    {
        if (second.type() == NATIVE)
            return true;
    }
    else if (second.type() == ISO4217)
    {
        if ((first.isoCI().issuer == second.isoCI().issuer) &&
            (first.isoCI().currencyCode == second.isoCI().currencyCode))
            return true;
    }
    return false;
}

void currencyCodeToStr(xdr::opaque_array<4U> const& code, std::string& retStr)
{
    retStr = "    ";
    for (int n = 0; n < 4; n++)
    {
        if (code[n])
            retStr[n] = code[n];
        else
        {
            retStr.resize(n);
            return;
        }
    }
}

void strToCurrencyCode(xdr::opaque_array<4U>& ret, std::string const& str)
{
    for (size_t n = 0; (n < str.size()) && (n < 4); n++)
    {
        ret[n] = str[n];
    }
}

// calculates A*B/C when A*B overflows 64bits
bool
bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C)
{
    // update when moving to (signed) int128
    assert((A >= 0) && (B >= 0) && (C > 0));
    uint128_t a(A);
    uint128_t b(B);
    uint128_t c(C);
    uint128_t x = (a * b) / c;

    result = (uint64_t)x;

    return (x <= INT64_MAX);
}

int64_t
bigDivide(int64_t A, int64_t B, int64_t C)
{
    int64_t res;
    if (!bigDivide(res, A, B, C))
    {
        throw std::overflow_error("overflow while performing bigDivide");
    }
    return res;
}

bool
iequals(std::string const& a, std::string const& b)
{
    size_t sz = a.size();
    if (b.size() != sz)
        return false;
    for (size_t i = 0; i < sz; ++i)
        if (tolower(a[i]) != tolower(b[i]))
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
