#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "xdrpp/message.h"
#include <vector>

namespace stellar
{
typedef std::vector<unsigned char> Blob;

bool isZero(uint256 const& b);

Hash& operator^=(Hash& l, Hash const& r);

// returns true if ( l ^ x ) < ( r ^ x)
bool lessThanXored(Hash const& l, Hash const& r, Hash const& x);

// returns true if the passed string32 is valid
bool isString32Valid(std::string const& str);

// returns true if the Asset value is well formed
bool isAssetValid(Asset const& cur);

// returns the issuer for the given asset
AccountID getIssuer(Asset const& asset);

// returns true if the currencies are the same
bool compareAsset(Asset const& first, Asset const& second);

template <uint32_t N>
void
assetCodeToStr(xdr::opaque_array<N> const& code, std::string& retStr)
{
    retStr.clear();
    for (auto c : code)
    {
        if (!c)
        {
            break;
        }
        retStr.push_back(c);
    }
};

template <uint32_t N>
void
strToAssetCode(xdr::opaque_array<N>& ret, std::string const& str)
{
    ret.fill(0);
    size_t n = std::min(ret.size(), str.size());
    std::copy(str.begin(), str.begin() + n, ret.begin());
}

bool addBalance(int64_t& balance, int64_t delta,
                int64_t maxBalance = std::numeric_limits<int64_t>::max());

enum Rounding
{
    ROUND_DOWN,
    ROUND_UP
};

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C, Rounding rounding);
// no throw version, returns true if result is valid
bool bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C,
               Rounding rounding);

// no throw version, returns true if result is valid
bool bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C,
               Rounding rounding);

bool iequals(std::string const& a, std::string const& b);

bool operator>=(Price const& a, Price const& b);
bool operator>(Price const& a, Price const& b);
bool operator==(Price const& a, Price const& b);
}
