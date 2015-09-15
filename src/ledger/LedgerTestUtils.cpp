// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "ledger/AccountFrame.h"
#include "crypto/SecretKey.h"
#include "util/types.h"
#include <string>
#include <cctype>
#include <xdrpp/autocheck.h>

namespace stellar
{
namespace LedgerTestUtils
{

template <typename T>
void
clampLow(T low, T& v)
{
    if (v < low)
    {
        v = low;
    }
}

template <typename T>
void
clampHigh(T high, T& v)
{
    if (v > high)
    {
        v = high;
    }
}


static void
stripControlCharacters(std::string& s)
{
    std::locale loc("C");

    for (auto it = s.begin(); it != s.end();)
    {
        char c = *it;
        if (c < 0 || std::iscntrl((char)c))
        {
            it = s.erase(it);
        }
        else
        {
            it++;
        }
    }
}

static bool
signerEqual(Signer const& s1, Signer const& s2)
{
    return s1.pubKey == s2.pubKey;
}

void
makeValid(AccountEntry& a)
{
    if (a.balance < 0)
    {
        a.balance = -a.balance;
    }

    stripControlCharacters(a.homeDomain);
    a.inflationDest.activate() = PubKeyUtils::random();

    std::sort(a.signers.begin(), a.signers.end(), &AccountFrame::signerCompare);
    a.signers.erase(
        std::unique(a.signers.begin(), a.signers.end(), signerEqual),
        a.signers.end());
    for (auto& s : a.signers)
    {
        if (s.weight == 0)
        {
            s.weight = 100;
        }
    }
    a.numSubEntries = (uint32)a.signers.size();
}

void
makeValid(TrustLineEntry& tl)
{
    tl.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
    clampLow<int64_t>(0, tl.balance);
    clampLow<int64_t>(0, tl.limit);
    clampHigh<int64_t>(tl.limit, tl.balance);
}
void
makeValid(OfferEntry& o)
{
    o.selling.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(o.selling.alphaNum4().assetCode, "CAD");

    o.buying.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(o.buying.alphaNum4().assetCode, "EUR");

    clampLow<int64_t>(0, o.amount);
    clampLow(0, o.price.n);
    clampLow(1, o.price.d);
}

static auto validLedgerEntryGenerator = autocheck::map(
    [](LedgerEntry&& le, size_t s)
    {
        auto& led = le.data;
        switch (led.type())
        {
        case TRUSTLINE:
            makeValid(led.trustLine());
            break;

        case OFFER:
            makeValid(led.offer());
            break;

        case ACCOUNT:
            makeValid(led.account());
            break;
        }

        return le;
    },
    autocheck::generator<LedgerEntry>());

static auto validAccountEntryGenerator = autocheck::map(
    [](AccountEntry&& ae, size_t s)
    {
        makeValid(ae);
        return ae;
    },
    autocheck::generator<AccountEntry>());

static auto validTrustLineEntryGenerator = autocheck::map(
    [](TrustLineEntry&& tl, size_t s)
    {
        makeValid(tl);
        return tl;
    },
    autocheck::generator<TrustLineEntry>());

static auto validOfferEntryGenerator = autocheck::map(
    [](OfferEntry&& o, size_t s)
    {
        makeValid(o);
        return o;
    },
    autocheck::generator<OfferEntry>());

LedgerEntry
generateValidLedgerEntry(size_t b)
{
    return validLedgerEntryGenerator(b);
}

std::vector<LedgerEntry>
generateValidLedgerEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validLedgerEntryGenerator);
    return vecgen(n);
}

AccountEntry
generateValidAccountEntry(size_t b)
{
    return validAccountEntryGenerator(b);
}

std::vector<AccountEntry>
generateValidAccountEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validAccountEntryGenerator);
    return vecgen(n);
}

TrustLineEntry
generateValidTrustLineEntry(size_t b)
{
    return validTrustLineEntryGenerator(b);
}

std::vector<TrustLineEntry>
generateValidTrustLineEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validTrustLineEntryGenerator);
    return vecgen(n);
}

OfferEntry
generateValidOfferEntry(size_t b)
{
    return validOfferEntryGenerator(b);
}

std::vector<OfferEntry>
generateValidOfferEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validOfferEntryGenerator);
    return vecgen(n);
}
}
}
