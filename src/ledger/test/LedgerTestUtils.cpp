// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/types.h"
#include <locale>
#include <string>
#include <xdrpp/autocheck.h>
#include <xdrpp/xdrpp/marshal.h>

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

// mutate string such that it doesn't contain control characters
// and is at least minSize characters long
template <typename T>
void
replaceControlCharacters(T& s, int minSize)
{
    auto& loc = std::locale::classic();
    if (static_cast<int>(s.size()) < minSize)
    {
        s.resize(minSize);
    }
    for (auto it = s.begin(); it != s.end(); it++)
    {
        char c = static_cast<char>(*it);
        if (c < 0 || std::iscntrl(c, loc))
        {
            auto b = autocheck::generator<char>{}(autocheck::detail::nalnums);
            *it = b;
        }
    }
}

static bool
signerEqual(Signer const& s1, Signer const& s2)
{
    return s1.key == s2.key;
}

void
makeValid(AccountEntry& a)
{
    if (a.balance < 0)
    {
        a.balance = -a.balance;
    }

    replaceControlCharacters(a.homeDomain, 0);

    if (a.inflationDest)
    {
        *a.inflationDest = PubKeyUtils::random();
    }

    std::sort(
        a.signers.begin(), a.signers.end(),
        [](Signer const& lhs, Signer const& rhs) { return lhs.key < rhs.key; });
    a.signers.erase(
        std::unique(a.signers.begin(), a.signers.end(), signerEqual),
        a.signers.end());
    for (auto& s : a.signers)
    {
        s.weight = s.weight & UINT8_MAX;
        if (s.weight == 0)
        {
            s.weight = 100;
        }
    }
    a.numSubEntries = (uint32)a.signers.size();
    if (a.seqNum < 0)
    {
        a.seqNum = -a.seqNum;
    }
    a.flags = a.flags & MASK_ACCOUNT_FLAGS;

    if (a.ext.v() == 1)
    {
        a.ext.v1().liabilities.buying = std::abs(a.ext.v1().liabilities.buying);
        a.ext.v1().liabilities.selling =
            std::abs(a.ext.v1().liabilities.selling);
    }
}

void
makeValid(TrustLineEntry& tl)
{
    if (tl.balance < 0)
    {
        tl.balance = -tl.balance;
    }
    tl.limit = std::abs(tl.limit);
    clampLow<int64>(1, tl.limit);
    tl.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
    clampHigh<int64_t>(tl.limit, tl.balance);
    tl.flags = tl.flags & MASK_TRUSTLINE_FLAGS;

    if (tl.ext.v() == 1)
    {
        tl.ext.v1().liabilities.buying =
            std::abs(tl.ext.v1().liabilities.buying);
        tl.ext.v1().liabilities.selling =
            std::abs(tl.ext.v1().liabilities.selling);
    }
}
void
makeValid(OfferEntry& o)
{
    o.offerID = o.offerID & INT64_MAX;
    o.selling.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(o.selling.alphaNum4().assetCode, "CAD");

    o.buying.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(o.buying.alphaNum4().assetCode, "EUR");

    o.amount = std::abs(o.amount);
    clampLow<int64>(1, o.amount);

    o.price.n = std::abs(o.price.n);
    o.price.d = std::abs(o.price.d);
    clampLow(1, o.price.n);
    clampLow(1, o.price.d);

    o.flags = o.flags & MASK_OFFERENTRY_FLAGS;
}

void
makeValid(DataEntry& d)
{
    replaceControlCharacters(d.dataName, 1);
}

void
makeValid(std::vector<LedgerHeaderHistoryEntry>& lhv,
          LedgerHeaderHistoryEntry firstLedger,
          HistoryManager::LedgerVerificationStatus state)
{
    auto randomIndex = rand_uniform<size_t>(1, lhv.size() - 1);
    auto prevHash = firstLedger.header.previousLedgerHash;
    auto ledgerSeq = firstLedger.header.ledgerSeq;

    for (auto i = 0; i < lhv.size(); i++)
    {
        auto& lh = lhv[i];

        lh.header.ledgerVersion = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        lh.header.previousLedgerHash = prevHash;
        lh.header.ledgerSeq = ledgerSeq;

        if (i == randomIndex && state != HistoryManager::VERIFY_STATUS_OK)
        {
            switch (state)
            {
            case HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION:
                lh.header.ledgerVersion += 1;
                break;
            case HistoryManager::VERIFY_STATUS_ERR_BAD_HASH:
                lh.header.previousLedgerHash = HashUtils::random();
                break;
            case HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT:
                lh.header.ledgerSeq -= 1;
                break;
            case HistoryManager::VERIFY_STATUS_ERR_OVERSHOT:
                lh.header.ledgerSeq += 1;
                break;
            default:
                break;
            }
        }
        // On a coin flip, corrupt header content rather than previous link
        autocheck::generator<bool> flip;
        if (i == randomIndex &&
            state == HistoryManager::VERIFY_STATUS_ERR_BAD_HASH && flip())
        {
            lh.hash = HashUtils::random();
        }
        else
        {
            lh.hash = sha256(xdr::xdr_to_opaque(lh.header));
        }

        prevHash = lh.hash;
        ledgerSeq = lh.header.ledgerSeq + 1;
    }

    if (state == HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES)
    {
        // Delete last element
        lhv.erase(lhv.begin() + lhv.size() - 1);
    }
}

static auto validLedgerEntryGenerator = autocheck::map(
    [](LedgerEntry&& le, size_t s) {
        auto& led = le.data;
        le.lastModifiedLedgerSeq = le.lastModifiedLedgerSeq & INT32_MAX;
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
        case DATA:
            makeValid(led.data());
            break;
        }

        return le;
    },
    autocheck::generator<LedgerEntry>());

static auto validAccountEntryGenerator = autocheck::map(
    [](AccountEntry&& ae, size_t s) {
        makeValid(ae);
        return ae;
    },
    autocheck::generator<AccountEntry>());

static auto validTrustLineEntryGenerator = autocheck::map(
    [](TrustLineEntry&& tl, size_t s) {
        makeValid(tl);
        return tl;
    },
    autocheck::generator<TrustLineEntry>());

static auto validOfferEntryGenerator = autocheck::map(
    [](OfferEntry&& o, size_t s) {
        makeValid(o);
        return o;
    },
    autocheck::generator<OfferEntry>());

static auto validDataEntryGenerator = autocheck::map(
    [](DataEntry&& d, size_t s) {
        makeValid(d);
        return d;
    },
    autocheck::generator<DataEntry>());

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

DataEntry
generateValidDataEntry(size_t b)
{
    return validDataEntryGenerator(b);
}

std::vector<DataEntry>
generateValidDataEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validDataEntryGenerator);
    return vecgen(n);
}

std::vector<LedgerHeaderHistoryEntry>
generateLedgerHeadersForCheckpoint(
    LedgerHeaderHistoryEntry firstLedger, uint32_t freq,
    HistoryManager::LedgerVerificationStatus state)
{
    static auto vecgen =
        autocheck::list_of(autocheck::generator<LedgerHeaderHistoryEntry>());
    auto res = vecgen(freq);
    makeValid(res, firstLedger, state);
    return res;
}
}
}
