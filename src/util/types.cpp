// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/types.h"
#include "lib/util/uint128_t.h"
#include <algorithm>
#include <locale>

namespace stellar
{
static std::locale cLocale("C");

using xdr::operator==;

bool
isZero(uint256 const& b)
{
    for (auto i : b)
        if (i != 0)
            return false;

    return true;
}

Hash&
operator^=(Hash& l, Hash const& r)
{
    std::transform(l.begin(), l.end(), r.begin(), l.begin(),
                   [](uint8_t a, uint8_t b) -> uint8_t { return a ^ b; });
    return l;
}

bool
lessThanXored(Hash const& l, Hash const& r, Hash const& x)
{
    Hash v1, v2;
    for (size_t i = 0; i < l.size(); i++)
    {
        v1[i] = x[i] ^ l[i];
        v2[i] = x[i] ^ r[i];
    }

    return v1 < v2;
}

bool
isString32Valid(std::string const& str)
{
    for (auto c : str)
    {
        if (c < 0 || std::iscntrl(c, cLocale))
        {
            return false;
        }
    }
    return true;
}

bool
isAssetValid(Asset const& cur)
{
    if (cur.type() == ASSET_TYPE_NATIVE)
        return true;

    if (cur.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        auto const& code = cur.alphaNum4().assetCode;
        bool zeros = false;
        bool onechar = false; // at least one non zero character
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
                if (b > 0x7F || !std::isalnum((char)b, cLocale))
                {
                    return false;
                }
                onechar = true;
            }
        }
        return onechar;
    }

    if (cur.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        auto const& code = cur.alphaNum12().assetCode;
        bool zeros = false;
        int charcount = 0; // at least 5 non zero characters
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
                if (b > 0x7F || !std::isalnum((char)b, cLocale))
                {
                    return false;
                }
                charcount++;
            }
        }
        return charcount > 4;
    }
    return false;
}

AccountID
getIssuer(Asset const& asset)
{
    return (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4
                ? asset.alphaNum4().issuer
                : asset.alphaNum12().issuer);
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

    if (second.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        if ((first.alphaNum12().issuer == second.alphaNum12().issuer) &&
            (first.alphaNum12().assetCode == second.alphaNum12().assetCode))
            return true;
    }
    return false;
}

bool
addBalance(int64_t& balance, int64_t delta, int64_t maxBalance)
{
    assert(balance >= 0);
    assert(maxBalance >= 0);

    if (delta == 0)
    {
        return true;
    }

    // strange-looking condition, but without UB
    // equivalent to (balance + delta) < 0
    // as balance >= 0, -balance > MIN_INT64, so no conversions needed
    if (delta < -balance)
    {
        return false;
    }

    if (maxBalance - balance < delta)
    {
        return false;
    }

    balance += delta;
    return true;
}

// calculates A*B/C when A*B overflows 64bits
bool
bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C, Rounding rounding)
{
    bool res;
    assert((A >= 0) && (B >= 0) && (C > 0));
    uint64_t r2;
    res = bigDivide(r2, (uint64_t)A, (uint64_t)B, (uint64_t)C, rounding);
    if (res)
    {
        res = r2 <= INT64_MAX;
        result = r2;
    }
    return res;
}

bool
bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C,
          Rounding rounding)
{
    // update when moving to (signed) int128
    uint128_t a(A);
    uint128_t b(B);
    uint128_t c(C);
    uint128_t x = rounding == ROUND_DOWN ? (a * b) / c : (a * b + c - 1) / c;

    result = (uint64_t)x;

    return (x <= UINT64_MAX);
}

int64_t
bigDivide(int64_t A, int64_t B, int64_t C, Rounding rounding)
{
    int64_t res;
    if (!bigDivide(res, A, B, C, rounding))
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

bool
operator>=(Price const& a, Price const& b)
{
    uint128_t l(a.n);
    uint128_t r(a.d);
    l *= b.d;
    r *= b.n;
    return l >= r;
}

bool
operator>(Price const& a, Price const& b)
{
    uint128_t l(a.n);
    uint128_t r(a.d);
    l *= b.d;
    r *= b.n;
    return l > r;
}

bool
operator==(Price const& a, Price const& b)
{
    return (a.n == b.n) && (a.d == b.d);
}
}
