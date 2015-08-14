#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>
#include "overlay/StellarXDR.h"
#include "xdrpp/message.h"

namespace stellar
{
typedef std::vector<unsigned char> Blob;

bool isZero(uint256 const& b);

uint256 makePublicKey(uint256 const& b);

// returns true if the Asset value is well formed
bool isAssetValid(Asset const& cur);

// returns true if the currencies are the same
bool compareAsset(Asset const& first, Asset const& second);

template<uint32_t N>
void assetCodeToStr(xdr::opaque_array<N> const& code, std::string& retStr){
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

template<uint32_t N>
void strToAssetCode(xdr::opaque_array<N>& ret, std::string const& str)
{
    ret.fill(0);
    size_t n = std::min(ret.size(), str.size());
    std::copy(str.begin(), str.begin() + n, ret.begin());
}

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C);
// no throw version, returns true if result is valid
bool bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C);

// no throw version, returns true if result is valid
bool bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C);

bool iequals(std::string const& a, std::string const& b);

bool operator>=(Price const& a, Price const& b);
bool operator>(Price const& a, Price const& b);
bool operator==(Price const& a, Price const& b);
}
