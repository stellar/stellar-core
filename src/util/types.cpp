// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/types.h"
#include "lib/util/format.h"
#include "lib/util/uint128_t.h"
#include "util/XDROperators.h"

#include <algorithm>
#include <locale>

namespace stellar
{

LedgerKey
LedgerEntryKey(LedgerEntry const& e)
{
    auto& d = e.data;
    LedgerKey k;
    switch (d.type())
    {

    case ACCOUNT:
        k.type(ACCOUNT);
        k.account().accountID = d.account().accountID;
        break;

    case TRUSTLINE:
        k.type(TRUSTLINE);
        k.trustLine().accountID = d.trustLine().accountID;
        k.trustLine().asset = d.trustLine().asset;
        break;

    case OFFER:
        k.type(OFFER);
        k.offer().sellerID = d.offer().sellerID;
        k.offer().offerID = d.offer().offerID;
        break;

    case DATA:
        k.type(DATA);
        k.data().accountID = d.data().accountID;
        k.data().dataName = d.data().dataName;
        break;
    }
    return k;
}

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
    auto& loc = std::locale::classic();
    for (auto c : str)
    {
        if (c < 0 || std::iscntrl(c, loc))
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

    auto& loc = std::locale::classic();
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
                if (b > 0x7F || !std::isalnum((char)b, loc))
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
                if (b > 0x7F || !std::isalnum((char)b, loc))
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

std::string
formatSize(size_t size)
{
    const std::vector<std::string> suffixes = {"B", "KB", "MB", "GB"};
    double dsize = static_cast<double>(size);

    auto i = 0;
    while (dsize >= 1024 && i < suffixes.size() - 1)
    {
        dsize /= 1024;
        i++;
    }

    return fmt::format("{:.2f}{}", dsize, suffixes[i]);
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
