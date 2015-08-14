// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/types.h"
#include "lib/util/uint128_t.h"
#include <locale>

namespace stellar
{
using xdr::operator==;

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
isAssetValid(Asset const& cur)
{
    if(cur.type() == ASSET_TYPE_NATIVE) return true;

    if (cur.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        auto const& code = cur.alphaNum4().assetCode;
        bool zeros = false;
        bool onechar = false; // at least one non zero character
        std::locale loc("C");
        for (uint8_t b : code)
        {
            if (b == 0)
            {
                zeros = true;
            }
            else if (zeros)
            {
                // zeros can only be trailing
                return false;
            }
            else
            {
                char t = *(char*)&b; // safe conversion to char
                if (!std::isalnum(t, loc))
                {
                    return false;
                }
                onechar = true;
            }
        }
        return onechar;
    }

    if(cur.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        auto const& code = cur.alphaNum12().assetCode;
        bool zeros = false;
        int charcount = 0; // at least 5 non zero characters
        std::locale loc("C");
        for(uint8_t b : code)
        {
            if(b == 0)
            {
                zeros = true;
            } else if(zeros)
            {
                // zeros can only be trailing
                return false;
            } else
            {
                char t = *(char*)&b; // safe conversion to char
                if(!std::isalnum(t, loc))
                {
                    return false;
                }
                charcount++;
            }
        }
        return charcount>4;
    }
    return false;


}

bool
compareAsset(Asset const& first, Asset const& second)
{
    if (first.type() != second.type())
        return false;

    if (first.type() == ASSET_TYPE_NATIVE)
        return true;

    if (second.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        if ((first.alphaNum4().issuer == second.alphaNum4().issuer) &&
            (first.alphaNum4().assetCode == second.alphaNum4().assetCode))
            return true;
    }

    if(second.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        if((first.alphaNum12().issuer == second.alphaNum12().issuer) &&
            (first.alphaNum12().assetCode == second.alphaNum12().assetCode))
            return true;
    }
    return false;
}

// calculates A*B/C when A*B overflows 64bits
bool
bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C)
{
    bool res;
    assert((A >= 0) && (B >= 0) && (C > 0));
    uint64_t r2;
    res = bigDivide(r2, (uint64_t)A, (uint64_t)B, (uint64_t)C);
    if (res)
    {
        res = r2 <= INT64_MAX;
        result = r2;
    }
    return res;
}

bool
bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C)
{
    // update when moving to (signed) int128
    uint128_t a(A);
    uint128_t b(B);
    uint128_t c(C);
    uint128_t x = (a * b) / c;

    result = (uint64_t)x;

    return (x <= UINT64_MAX);
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

bool operator>=(Price const& a, Price const& b)
{
    uint128_t l(a.n);
    uint128_t r(a.d);
    l *= b.d;
    r *= b.n;
    return l >= r;
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
