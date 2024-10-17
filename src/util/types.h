#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-types.h"
#include <set>
#include <vector>

namespace stellar
{
typedef std::vector<unsigned char> Blob;

typedef std::set<LedgerKey, LedgerEntryIdCmp> LedgerKeySet;

LedgerKey LedgerEntryKey(LedgerEntry const& e);

bool isZero(uint256 const& b);

Hash& operator^=(Hash& l, Hash const& r);

// returns true if ( l ^ x ) < ( r ^ x)
bool lessThanXored(Hash const& l, Hash const& r, Hash const& x);

// returns true if the passed string is valid
bool isStringValid(std::string const& str);

// returns true if the currencies are the same
bool compareAsset(Asset const& first, Asset const& second);

// returns the int32_t of a non-negative uint32_t if it fits,
// otherwise throws.
int32_t unsignedToSigned(uint32_t v);

// returns the int64_t of a non-negative uint64_t if it fits,
// otherwise throws.
int64_t unsignedToSigned(uint64_t v);

std::string formatSize(size_t size);

// returns true if the asset is well formed for the specified protocol version
template <typename T> bool isAssetValid(T const& cur, uint32_t ledgerVersion);

// returns the issuer for the given asset
template <typename T>
AccountID
getIssuer(T const& asset)
{
    switch (asset.type())
    {
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        return asset.alphaNum4().issuer;
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        return asset.alphaNum12().issuer;
    case ASSET_TYPE_NATIVE:
    case ASSET_TYPE_POOL_SHARE:
        throw std::runtime_error("asset does not have an issuer");
    default:
        throw std::runtime_error("unknown asset type");
    }
}

template <typename T>
bool
isIssuer(AccountID const& acc, T const& asset)
{
    switch (asset.type())
    {
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        return acc == asset.alphaNum4().issuer;
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        return acc == asset.alphaNum12().issuer;
    case ASSET_TYPE_NATIVE:
    case ASSET_TYPE_POOL_SHARE:
        return false;
    default:
        throw std::runtime_error("unknown asset type");
    }
}

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

inline std::string
assetToString(const Asset& asset)
{
    auto r = std::string{};
    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        r = std::string{"XLM"};
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        assetCodeToStr(asset.alphaNum4().assetCode, r);
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        assetCodeToStr(asset.alphaNum12().assetCode, r);
        break;
    case stellar::ASSET_TYPE_POOL_SHARE:
        throw std::runtime_error(
            "ASSET_TYPE_POOL_SHARE is not a valid Asset type");
    }
    return r;
};

inline LedgerKey
getBucketLedgerKey(HotArchiveBucketEntry const& be)
{
    switch (be.type())
    {
    case HOT_ARCHIVE_LIVE:
    case HOT_ARCHIVE_DELETED:
        return be.key();
    case HOT_ARCHIVE_ARCHIVED:
        return LedgerEntryKey(be.archivedEntry());
    case HOT_ARCHIVE_METAENTRY:
    default:
        throw std::invalid_argument("Tried to get key for METAENTRY");
    }
}

inline LedgerKey
getBucketLedgerKey(BucketEntry const& be)
{
    switch (be.type())
    {
    case LIVEENTRY:
    case INITENTRY:
        return LedgerEntryKey(be.liveEntry());
    case DEADENTRY:
        return be.deadEntry();
    case METAENTRY:
    default:
        throw std::invalid_argument("Tried to get key for METAENTRY");
    }
}

// Round value v down to largest multiple of m, m must be power of 2
template <typename T>
inline T
roundDown(T v, T m)
{
    return v & ~(m - 1);
}

bool addBalance(int64_t& balance, int64_t delta,
                int64_t maxBalance = std::numeric_limits<int64_t>::max());

bool iequals(std::string const& a, std::string const& b);

bool operator>=(Price const& a, Price const& b);
bool operator>(Price const& a, Price const& b);
bool operator==(Price const& a, Price const& b);
}
