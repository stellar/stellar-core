#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/uint128_t.h"
#include "numeric.h"
#include "overlay/StellarXDR.h"
#include "xdrpp/message.h"
#include <type_traits>
#include <vector>

namespace stellar
{
typedef std::vector<unsigned char> Blob;

LedgerKey LedgerEntryKey(LedgerEntry const& e);

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

std::string formatSize(size_t size);

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

bool iequals(std::string const& a, std::string const& b);

bool operator>=(Price const& a, Price const& b);
bool operator>(Price const& a, Price const& b);
bool operator==(Price const& a, Price const& b);
}
