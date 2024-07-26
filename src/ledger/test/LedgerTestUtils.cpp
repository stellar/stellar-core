// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"
#include <autocheck/generator.hpp>
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

template <size_t MAX_SIZE>
xdr::xvector<uint8_t, MAX_SIZE>
generateOpaqueVector()
{
    static auto vecgen = autocheck::list_of(autocheck::generator<uint8_t>());
    stellar::uniform_int_distribution<size_t> distr(1, MAX_SIZE);
    auto vec = vecgen(distr(autocheck::rng()));
    return xdr::xvector<uint8_t, MAX_SIZE>(vec.begin(), vec.end());
}

void
randomlyModifyEntry(LedgerEntry& e)
{
    switch (e.data.type())
    {
    case TRUSTLINE:
        e.data.trustLine().limit = autocheck::generator<int64>{}();
        e.data.trustLine().balance = e.data.trustLine().limit - 10;
        makeValid(e.data.trustLine());
        break;
    case OFFER:
        e.data.offer().amount = autocheck::generator<int64>{}();
        makeValid(e.data.offer());
        break;
    case ACCOUNT:
        e.data.account().balance = autocheck::generator<int64>{}();
        e.data.account().seqNum++;
        makeValid(e.data.account());
        break;
    case DATA:
        e.data.data().dataValue = autocheck::generator<DataValue>{}(8);
        makeValid(e.data.data());
        break;
    case CLAIMABLE_BALANCE:
        e.data.claimableBalance().amount = autocheck::generator<int64>{}();
        makeValid(e.data.claimableBalance());
        break;
    case LIQUIDITY_POOL:
        e.data.liquidityPool().body.constantProduct().reserveA =
            autocheck::generator<int64>{}();
        e.data.liquidityPool().body.constantProduct().reserveB =
            autocheck::generator<int64>{}();
        e.data.liquidityPool().body.constantProduct().totalPoolShares =
            autocheck::generator<int64>{}();
        e.data.liquidityPool().body.constantProduct().poolSharesTrustLineCount =
            autocheck::generator<int64>{}();
        makeValid(e.data.liquidityPool());
        break;
    case CONFIG_SETTING:
    {
        e.data.configSetting().configSettingID(
            CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES);
        e.data.configSetting().contractMaxSizeBytes() =
            autocheck::generator<uint32_t>{}();
        makeValid(e.data.configSetting());
        break;
    }
    case CONTRACT_DATA:
        e.data.contractData().val.type(SCV_I32);
        e.data.contractData().val.i32() = autocheck::generator<int32_t>{}();
        makeValid(e.data.contractData());
        break;
    case CONTRACT_CODE:
    {
        auto code = generateOpaqueVector<60000>();
        e.data.contractCode().code.assign(code.begin(), code.end());
        makeValid(e.data.contractCode());
        break;
    }
    case TTL:
        e.data.ttl().liveUntilLedgerSeq = autocheck::generator<uint32_t>{}();
        break;
    }
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
        *a.inflationDest = PubKeyUtils::pseudoRandomForTesting();
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
    a.flags = a.flags & MASK_ACCOUNT_FLAGS_V17;

    if (a.ext.v() == 1)
    {
        a.ext.v1().liabilities.buying = std::abs(a.ext.v1().liabilities.buying);
        a.ext.v1().liabilities.selling =
            std::abs(a.ext.v1().liabilities.selling);

        if (a.ext.v1().ext.v() == 2)
        {
            auto& extV2 = a.ext.v1().ext.v2();

            int64_t effEntries = 2LL;
            effEntries += a.numSubEntries;
            effEntries += extV2.numSponsoring;
            effEntries -= extV2.numSponsored;
            if (effEntries < 0)
            {
                // This condition implies (in arbitrary precision)
                //      2 + numSubentries + numSponsoring - numSponsored < 0
                // which can be rearranged as
                //      numSponsored > 2 + numSubentries + numSponsoring .
                // Substituting this inequality yields
                //      2 + numSubentries + numSponsored
                //          > 4 + 2 * numSubentries + numSponsoring
                //          > numSponsoring
                // which can be rearranged as
                //      2 + numSubentries + numSponsored - numSponsoring > 0 .
                // In summary, swapping numSponsored and numSponsoring fixes the
                // account state.
                std::swap(extV2.numSponsored, extV2.numSponsoring);
            }

            extV2.signerSponsoringIDs.resize(
                static_cast<uint32_t>(a.signers.size()));
        }
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

    switch (tl.asset.type())
    {
    case ASSET_TYPE_NATIVE:
        // ASSET_TYPE_NATIVE is not a valid trustline asset type, so change the
        // type to a valid one
        tl.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
        break;
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
        break;
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        strToAssetCode(tl.asset.alphaNum12().assetCode, "USD12");
        break;
    default:
        break;
    }

    clampHigh<int64_t>(tl.limit, tl.balance);
    tl.flags = tl.flags & MASK_TRUSTLINE_FLAGS_V17;

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
makeValid(ClaimableBalanceEntry& c)
{
    c.amount = std::abs(c.amount);
    clampLow<int64>(1, c.amount);

    // It is not valid for claimants to be empty, so if this occurs we default
    // to a single claimant for the zero account with
    // CLAIM_PREDICATE_UNCONDITIONAL.
    if (c.claimants.empty())
    {
        c.claimants.resize(1);
    }

    c.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(c.asset.alphaNum4().assetCode, "CAD");

    if (c.ext.v() == 1)
    {
        c.ext.v1().flags = MASK_CLAIMABLE_BALANCE_FLAGS;
    }
}

void
makeValid(LiquidityPoolEntry& lp)
{
    auto& cp = lp.body.constantProduct();
    cp.params.assetA.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cp.params.assetA.alphaNum4().assetCode, "CAD");

    cp.params.assetB.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cp.params.assetB.alphaNum4().assetCode, "USD");

    cp.params.fee = 30;
    cp.reserveA = std::abs(cp.reserveA);
    cp.reserveB = std::abs(cp.reserveB);
    cp.totalPoolShares = std::abs(cp.totalPoolShares);
    cp.poolSharesTrustLineCount = std::abs(cp.poolSharesTrustLineCount);
}

void
makeValid(ConfigSettingEntry& ce)
{
    auto ids = xdr::xdr_traits<ConfigSettingID>::enum_values();
    ce.configSettingID(static_cast<ConfigSettingID>(
        ids.at(ce.configSettingID() % ids.size())));
}

void
makeValid(ContractDataEntry& cde)
{
    int t = cde.durability;
    auto modulo = static_cast<int64_t>(
        xdr::xdr_traits<ContractDataDurability>::enum_values().size());
    cde.durability = static_cast<ContractDataDurability>(std::abs(t % modulo));

    LedgerEntry le;
    le.data.type(CONTRACT_DATA);
    le.data.contractData() = cde;

    auto key = LedgerEntryKey(le);
    if (xdr::xdr_size(key) >
        InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES)
    {
        // make the key small to prevent hitting the limit
        static const uint32_t key_limit =
            InitialSorobanNetworkConfig::MAX_CONTRACT_DATA_KEY_SIZE_BYTES - 50;
        auto small_bytes =
            autocheck::generator<xdr::opaque_vec<key_limit>>()(5);
        SCVal val(SCV_BYTES);
        val.bytes().assign(small_bytes.begin(), small_bytes.end());
        cde.key = val;
    }
    // Fix the error values.
    // NB: The internal SCErrors in maps/vecs still may be invalid.
    // We might want to fix that eventually, but in general a significant
    // (~80-90%) fraction of generated entries will be valid XDR.
    if (cde.key.type() == SCV_ERROR)
    {
        if (cde.key.error().type() != SCErrorType::SCE_CONTRACT)
        {
            cde.key.error().code() = static_cast<SCErrorCode>(
                std::abs(cde.key.error().code()) %
                xdr::xdr_traits<SCErrorCode>::enum_values().size());
        }
    }
}

void
makeValid(ContractCodeEntry& cce)
{
}

void
makeValid(TTLEntry& cce)
{
}

void
makeValid(std::vector<LedgerHeaderHistoryEntry>& lhv,
          LedgerHeaderHistoryEntry firstLedger,
          HistoryManager::LedgerVerificationStatus state)
{
    // We want to avoid corrupting the 0th through 2nd entries, because we use
    // these corrupt sequences in history tests that differentiate between
    // failures that encounter local state, specifically the LCL (suggesting the
    // local node has diverged), and those that don't (suggesting merely corrupt
    // download material), and the LCL-encounters in these tests happen at
    // ledger entry 64, entry 0 in the checkpoint.
    //
    // An undershot corruption at entry 2 will cause verification of to
    // interpret it as entry 1 with a wrong prev-ptr pointing to entry 0 which
    // is LCL, causing an LCL encounter. Any other corruption at entry 1 will
    // similarly cause an LCL encounter. Corruptions at or beyond entry 3 are ok
    // though.
    releaseAssertOrThrow(lhv.size() > 3);
    auto randomIndex = rand_uniform<size_t>(3, lhv.size() - 1);
    auto prevHash = firstLedger.header.previousLedgerHash;
    auto ledgerSeq = firstLedger.header.ledgerSeq;

    for (size_t i = 0; i < lhv.size(); i++)
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
                lh.header.previousLedgerHash =
                    HashUtils::pseudoRandomForTesting();
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
        // On a coin flip, corrupt header as well as previous link
        if (i == randomIndex &&
            state == HistoryManager::VERIFY_STATUS_ERR_BAD_HASH && rand_flip())
        {
            lh.hash = HashUtils::pseudoRandomForTesting();
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
        case ACCOUNT:
            makeValid(led.account());
            break;
        case TRUSTLINE:
            makeValid(led.trustLine());
            break;
        case OFFER:
            makeValid(led.offer());
            break;
        case DATA:
            makeValid(led.data());
            break;
        case CLAIMABLE_BALANCE:
            makeValid(led.claimableBalance());
            break;
        case LIQUIDITY_POOL:
            makeValid(led.liquidityPool());
            break;
        case CONFIG_SETTING:
            makeValid(led.configSetting());
            break;
        case CONTRACT_DATA:
            makeValid(led.contractData());
            break;
        case CONTRACT_CODE:
            makeValid(led.contractCode());
            break;
        case TTL:
            makeValid(led.ttl());
            break;
        }

        return std::move(le);
    },
    autocheck::generator<LedgerEntry>());

static auto ledgerKeyGenerator = autocheck::such_that(
    [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
    autocheck::generator<LedgerKey>());

static auto validAccountEntryGenerator = autocheck::map(
    [](AccountEntry&& ae, size_t s) {
        makeValid(ae);
        return std::move(ae);
    },
    autocheck::generator<AccountEntry>());

static auto validTrustLineEntryGenerator = autocheck::map(
    [](TrustLineEntry&& tl, size_t s) {
        makeValid(tl);
        return std::move(tl);
    },
    autocheck::generator<TrustLineEntry>());

static auto validOfferEntryGenerator = autocheck::map(
    [](OfferEntry&& o, size_t s) {
        makeValid(o);
        return std::move(o);
    },
    autocheck::generator<OfferEntry>());

static auto validDataEntryGenerator = autocheck::map(
    [](DataEntry&& d, size_t s) {
        makeValid(d);
        return std::move(d);
    },
    autocheck::generator<DataEntry>());

static auto validClaimableBalanceEntryGenerator = autocheck::map(
    [](ClaimableBalanceEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ClaimableBalanceEntry>());

static auto validLiquidityPoolEntryGenerator = autocheck::map(
    [](LiquidityPoolEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<LiquidityPoolEntry>());

static auto validConfigSettingEntryGenerator = autocheck::map(
    [](ConfigSettingEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ConfigSettingEntry>());

static auto validContractDataEntryGenerator = autocheck::map(
    [](ContractDataEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ContractDataEntry>());

static auto validContractCodeEntryGenerator = autocheck::map(
    [](ContractCodeEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ContractCodeEntry>());

static auto validTTLEntryGenerator = autocheck::map(
    [](TTLEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<TTLEntry>());

LedgerEntry
generateValidLedgerEntry(size_t b)
{
    return validLedgerEntryGenerator(b);
}

LedgerEntry
generateValidLedgerEntryOfType(LedgerEntryType type)
{
    auto entry = generateValidLedgerEntry();
    while (entry.data.type() != type)
    {
        entry = generateValidLedgerEntry();
    }
    return entry;
}

std::vector<LedgerEntry>
generateValidLedgerEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validLedgerEntryGenerator);
    return vecgen(n);
}

std::vector<LedgerEntry>
generateValidUniqueLedgerEntries(size_t n)
{
    UnorderedSet<LedgerKey> keys;
    std::vector<LedgerEntry> entries;
    while (entries.size() < n)
    {
        auto entry = generateValidLedgerEntry();
        auto key = LedgerEntryKey(entry);
        if (keys.find(key) != keys.end())
        {
            continue;
        }
        keys.insert(key);
        entries.push_back(entry);
    }
    return entries;
}

LedgerEntry
generateValidLedgerEntryWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t b)
{
    while (true)
    {
        auto entry = generateValidLedgerEntry(b);
        if (excludedTypes.find(entry.data.type()) == excludedTypes.end())
        {
            return entry;
        }
    }
}

std::vector<LedgerEntry>
generateValidLedgerEntriesWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n)
{

    if (n > 1000)
    {
        throw "generateValidLedgerEntryWithExclusions: must generate <= 1000 "
              "entries";
    }

    std::vector<LedgerEntry> res;
    res.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        res.push_back(generateValidLedgerEntryWithExclusions(excludedTypes));
    }
    return res;
}

std::vector<LedgerKey>
generateValidLedgerEntryKeysWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n)
{
    if (n > 1000)
    {
        throw "generateValidLedgerEntryKeysWithExclusions: must generate <= "
              "1000 entries";
    }

    auto entries = LedgerTestUtils::generateValidLedgerEntriesWithExclusions(
        excludedTypes, n);
    std::vector<LedgerKey> keys;
    keys.reserve(entries.size());
    for (auto const& entry : entries)
    {
        keys.push_back(LedgerEntryKey(entry));
    }
    return keys;
}

std::vector<LedgerKey>
generateUniqueValidSorobanLedgerEntryKeys(size_t n)
{
    return LedgerTestUtils::generateValidUniqueLedgerEntryKeysWithExclusions(
        {OFFER, DATA, CLAIMABLE_BALANCE, LIQUIDITY_POOL, CONFIG_SETTING, TTL},
        n);
}

std::vector<LedgerKey>
generateValidUniqueLedgerEntryKeysWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n)
{
    UnorderedSet<LedgerKey> keys;
    std::vector<LedgerKey> res;
    keys.reserve(n);
    res.reserve(n);
    while (keys.size() < n)
    {
        auto entry = generateValidLedgerEntryWithExclusions(excludedTypes, n);
        auto key = LedgerEntryKey(entry);
        if (keys.find(key) != keys.end())
        {
            continue;
        }

        keys.insert(key);
        res.emplace_back(key);
    }
    return res;
}

std::vector<LedgerEntry>
generateValidUniqueLedgerEntriesWithExclusions(
    std::unordered_set<LedgerEntryType> const& excludedTypes, size_t n)
{
    UnorderedSet<LedgerKey> keys;
    std::vector<LedgerEntry> res;
    keys.reserve(n);
    res.reserve(n);
    while (keys.size() < n)
    {
        auto entry = generateValidLedgerEntryWithExclusions(excludedTypes, n);
        auto key = LedgerEntryKey(entry);
        if (keys.find(key) != keys.end())
        {
            continue;
        }

        keys.insert(key);
        res.emplace_back(entry);
    }
    return res;
}

LedgerEntry
generateValidLedgerEntryWithTypes(
    std::unordered_set<LedgerEntryType> const& types, size_t b)
{
    while (true)
    {
        auto entry = generateValidLedgerEntry(b);
        if (types.find(entry.data.type()) != types.end())
        {
            return entry;
        }
    }
}

std::vector<LedgerKey>
generateValidUniqueLedgerKeysWithTypes(
    std::unordered_set<LedgerEntryType> const& types, size_t n,
    UnorderedSet<LedgerKey>& seenKeys)
{
    std::vector<LedgerKey> res;
    res.reserve(n);
    while (res.size() < n)
    {

        auto entry = generateValidLedgerEntryWithTypes(types);
        auto key = LedgerEntryKey(entry);
        if (seenKeys.find(key) != seenKeys.end())
        {
            continue;
        }

        seenKeys.insert(key);
        res.emplace_back(key);
    }
    return res;
}

std::vector<LedgerEntry>
generateValidUniqueLedgerEntriesWithTypes(
    std::unordered_set<LedgerEntryType> const& types, size_t n)
{
    UnorderedSet<LedgerKey> keys;
    std::vector<LedgerEntry> entries;
    entries.reserve(n);
    keys.reserve(n);
    while (entries.size() < n)
    {
        auto entry = generateValidLedgerEntryWithTypes(types);
        auto key = LedgerEntryKey(entry);
        if (keys.find(key) != keys.end())
        {
            continue;
        }

        keys.insert(key);
        entries.push_back(entry);
    }
    return entries;
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
generateNonPoolShareValidTrustLineEntry(size_t b)
{
    auto tl = validTrustLineEntryGenerator(b);
    if (tl.asset.type() == ASSET_TYPE_POOL_SHARE)
    {
        tl.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
    }

    return tl;
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

ClaimableBalanceEntry
generateValidClaimableBalanceEntry(size_t b)
{
    return validClaimableBalanceEntryGenerator(b);
}

std::vector<ClaimableBalanceEntry>
generateValidClaimableBalanceEntries(size_t n)
{
    static auto vecgen =
        autocheck::list_of(validClaimableBalanceEntryGenerator);
    return vecgen(n);
}

LiquidityPoolEntry
generateValidLiquidityPoolEntry(size_t b)
{
    return validLiquidityPoolEntryGenerator(b);
}

std::vector<LiquidityPoolEntry>
generateValidLiquidityPoolEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validLiquidityPoolEntryGenerator);
    return vecgen(n);
}

ConfigSettingEntry
generateValidConfigSettingEntry(size_t b)
{
    return validConfigSettingEntryGenerator(b);
}

std::vector<ConfigSettingEntry>
generateValidConfigSettingEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validConfigSettingEntryGenerator);
    return vecgen(n);
}

ContractDataEntry
generateValidContractDataEntry(size_t b)
{
    return validContractDataEntryGenerator(b);
}

std::vector<ContractDataEntry>
generateValidContractDataEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validContractDataEntryGenerator);
    return vecgen(n);
}

ContractCodeEntry
generateValidContractCodeEntry(size_t b)
{
    return validContractCodeEntryGenerator(b);
}

std::vector<ContractCodeEntry>
generateValidContractCodeEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validContractCodeEntryGenerator);
    return vecgen(n);
}

TTLEntry
generateValidTTLEntry(size_t b)
{
    return validTTLEntryGenerator(b);
}

std::vector<TTLEntry>
generateValidTTLEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validTTLEntryGenerator);
    return vecgen(n);
}

std::vector<LedgerHeaderHistoryEntry>
generateLedgerHeadersForCheckpoint(
    LedgerHeaderHistoryEntry firstLedger, uint32_t size,
    HistoryManager::LedgerVerificationStatus state)
{
    static auto vecgen =
        autocheck::list_of(autocheck::generator<LedgerHeaderHistoryEntry>());
    auto res = vecgen(size);
    makeValid(res, firstLedger, state);
    return res;
}

UpgradeType
toUpgradeType(LedgerUpgrade const& upgrade)
{
    auto v = xdr::xdr_to_opaque(upgrade);
    auto result = UpgradeType{v.begin(), v.end()};
    return result;
}
}
}
