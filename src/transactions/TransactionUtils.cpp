// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionUtils.h"
#include "crypto/SHA.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/OfferExchange.h"
#include "transactions/SponsorshipUtils.h"
#include "util/ProtocolVersion.h"
#include "util/types.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include <Tracy.hpp>

namespace stellar
{

#ifdef BUILD_TESTS
#define PROD_CONST
#else
#define PROD_CONST const
#endif

static uint32_t PROD_CONST ACCOUNT_SUBENTRY_LIMIT = 1000;
static size_t PROD_CONST MAX_OFFERS_TO_CROSS = 1000;

uint32_t
getAccountSubEntryLimit()
{
    return ACCOUNT_SUBENTRY_LIMIT;
}

size_t
getMaxOffersToCross()
{
    return MAX_OFFERS_TO_CROSS;
}

#ifdef BUILD_TESTS
TempReduceLimitsForTesting::TempReduceLimitsForTesting(
    uint32_t accountSubEntryLimit, size_t maxOffersToCross)
    : mOldAccountSubEntryLimit(ACCOUNT_SUBENTRY_LIMIT)
    , mOldMaxOffersToCross(MAX_OFFERS_TO_CROSS)
{
    ACCOUNT_SUBENTRY_LIMIT = accountSubEntryLimit;
    MAX_OFFERS_TO_CROSS = maxOffersToCross;
}

TempReduceLimitsForTesting::~TempReduceLimitsForTesting()
{
    ACCOUNT_SUBENTRY_LIMIT = mOldAccountSubEntryLimit;
    MAX_OFFERS_TO_CROSS = mOldMaxOffersToCross;
}
#endif

AccountEntryExtensionV1&
prepareAccountEntryExtensionV1(AccountEntry& ae)
{
    if (ae.ext.v() == 0)
    {
        ae.ext.v(1);
        ae.ext.v1().liabilities = Liabilities{0, 0};
    }
    return ae.ext.v1();
}

AccountEntryExtensionV2&
prepareAccountEntryExtensionV2(AccountEntry& ae)
{
    auto& extV1 = prepareAccountEntryExtensionV1(ae);
    if (extV1.ext.v() == 0)
    {
        extV1.ext.v(2);
        auto& extV2 = extV1.ext.v2();
        extV2.signerSponsoringIDs.resize(
            static_cast<uint32_t>(ae.signers.size()));
    }
    return extV1.ext.v2();
}

AccountEntryExtensionV3&
prepareAccountEntryExtensionV3(AccountEntry& ae)
{
    auto& extV2 = prepareAccountEntryExtensionV2(ae);
    if (extV2.ext.v() == 0)
    {
        extV2.ext.v(3);
        auto& extV3 = extV2.ext.v3();
        extV3.seqLedger = 0;
        extV3.seqTime = 0;
    }
    return extV2.ext.v3();
}

TrustLineEntry::_ext_t::_v1_t&
prepareTrustLineEntryExtensionV1(TrustLineEntry& tl)
{
    if (tl.ext.v() == 0)
    {
        tl.ext.v(1);
        tl.ext.v1().liabilities = Liabilities{0, 0};
    }
    return tl.ext.v1();
}

TrustLineEntryExtensionV2&
prepareTrustLineEntryExtensionV2(TrustLineEntry& tl)
{
    auto& extV1 = prepareTrustLineEntryExtensionV1(tl);

    if (extV1.ext.v() == 0)
    {
        extV1.ext.v(2);
        extV1.ext.v2().liquidityPoolUseCount = 0;
    }
    return extV1.ext.v2();
}

LedgerEntryExtensionV1&
prepareLedgerEntryExtensionV1(LedgerEntry& le)
{
    if (le.ext.v() == 0)
    {
        le.ext.v(1);
        le.ext.v1().sponsoringID.reset();
    }
    return le.ext.v1();
}

void
setLedgerHeaderFlag(LedgerHeader& lh, uint32_t flags)
{
    if (lh.ext.v() == 0)
    {
        lh.ext.v(1);
    }

    lh.ext.v1().flags = flags;
}

AccountEntryExtensionV2&
getAccountEntryExtensionV2(AccountEntry& ae)
{
    if (ae.ext.v() != 1 || ae.ext.v1().ext.v() != 2)
    {
        throw std::runtime_error("expected AccountEntry extension V2");
    }
    return ae.ext.v1().ext.v2();
}

AccountEntryExtensionV3 const&
getAccountEntryExtensionV3(AccountEntry const& ae)
{
    if (ae.ext.v() != 1 || ae.ext.v1().ext.v() != 2 ||
        ae.ext.v1().ext.v2().ext.v() != 3)
    {
        throw std::runtime_error("expected AccountEntry extension V3");
    }
    return ae.ext.v1().ext.v2().ext.v3();
}

TrustLineEntryExtensionV2&
getTrustLineEntryExtensionV2(TrustLineEntry& tl)
{
    if (!hasTrustLineEntryExtV2(tl))
    {
        throw std::runtime_error("expected TrustLineEntry extension V2");
    }

    return tl.ext.v1().ext.v2();
}

LedgerEntryExtensionV1&
getLedgerEntryExtensionV1(LedgerEntry& le)
{
    if (le.ext.v() != 1)
    {
        throw std::runtime_error("expected LedgerEntry extension V1");
    }

    return le.ext.v1();
}

static bool
checkAuthorization(LedgerHeader const& header, LedgerEntry const& entry)
{
    if (protocolVersionIsBefore(header.ledgerVersion, ProtocolVersion::V_10))
    {
        if (!isAuthorized(entry))
        {
            return false;
        }
    }
    else if (!isAuthorizedToMaintainLiabilities(entry))
    {
        throw std::runtime_error("Invalid authorization");
    }

    return true;
}

LedgerKey
accountKey(AccountID const& accountID)
{
    LedgerKey key(ACCOUNT);
    key.account().accountID = accountID;
    return key;
}

LedgerKey
trustlineKey(AccountID const& accountID, Asset const& asset)
{
    return trustlineKey(accountID, assetToTrustLineAsset(asset));
}

LedgerKey
trustlineKey(AccountID const& accountID, TrustLineAsset const& asset)
{
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = accountID;
    key.trustLine().asset = asset;
    return key;
}

LedgerKey
offerKey(AccountID const& sellerID, uint64_t offerID)
{
    LedgerKey key(OFFER);
    key.offer().sellerID = sellerID;
    key.offer().offerID = offerID;
    return key;
}

LedgerKey
dataKey(AccountID const& accountID, std::string const& dataName)
{
    LedgerKey key(DATA);
    key.data().accountID = accountID;
    key.data().dataName = dataName;
    return key;
}

LedgerKey
claimableBalanceKey(ClaimableBalanceID const& balanceID)
{
    LedgerKey key(CLAIMABLE_BALANCE);
    key.claimableBalance().balanceID = balanceID;
    return key;
}

LedgerKey
liquidityPoolKey(PoolID const& poolID)
{
    LedgerKey key(LIQUIDITY_POOL);
    key.liquidityPool().liquidityPoolID = poolID;
    return key;
}

LedgerKey
poolShareTrustLineKey(AccountID const& accountID, PoolID const& poolID)
{
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = accountID;
    key.trustLine().asset.type(ASSET_TYPE_POOL_SHARE);
    key.trustLine().asset.liquidityPoolID() = poolID;
    return key;
}

LedgerKey
configSettingKey(ConfigSettingID const& configSettingID)
{
    LedgerKey key(CONFIG_SETTING);
    key.configSetting().configSettingID = configSettingID;
    return key;
}

LedgerKey
contractDataKey(SCAddress const& contract, SCVal const& dataKey,
                ContractDataDurability durability)
{
    LedgerKey key(CONTRACT_DATA);
    key.contractData().contract = contract;
    key.contractData().key = dataKey;
    key.contractData().durability = durability;
    return key;
}

LedgerKey
contractCodeKey(Hash const& hash)
{
    LedgerKey key(CONTRACT_CODE);
    key.contractCode().hash = hash;
    return key;
}

InternalLedgerKey
sponsorshipKey(AccountID const& sponsoredID)
{
    return InternalLedgerKey::makeSponsorshipKey(sponsoredID);
}

InternalLedgerKey
sponsorshipCounterKey(AccountID const& sponsoringID)
{
    return InternalLedgerKey::makeSponsorshipCounterKey(sponsoringID);
}

InternalLedgerKey
maxSeqNumToApplyKey(AccountID const& sourceAccount)
{
    return InternalLedgerKey::makeMaxSeqNumToApplyKey(sourceAccount);
}

LedgerTxnEntry
loadAccount(AbstractLedgerTxn& ltx, AccountID const& accountID)
{
    ZoneScoped;
    return ltx.load(accountKey(accountID));
}

ConstLedgerTxnEntry
loadAccountWithoutRecord(AbstractLedgerTxn& ltx, AccountID const& accountID)
{
    ZoneScoped;
    return ltx.loadWithoutRecord(accountKey(accountID));
}

LedgerTxnEntry
loadData(AbstractLedgerTxn& ltx, AccountID const& accountID,
         std::string const& dataName)
{
    ZoneScoped;
    return ltx.load(dataKey(accountID, dataName));
}

LedgerTxnEntry
loadOffer(AbstractLedgerTxn& ltx, AccountID const& sellerID, int64_t offerID)
{
    ZoneScoped;
    return ltx.load(offerKey(sellerID, offerID));
}

LedgerTxnEntry
loadClaimableBalance(AbstractLedgerTxn& ltx,
                     ClaimableBalanceID const& balanceID)
{
    ZoneScoped;
    return ltx.load(claimableBalanceKey(balanceID));
}

TrustLineWrapper
loadTrustLine(AbstractLedgerTxn& ltx, AccountID const& accountID,
              Asset const& asset)
{
    ZoneScoped;
    return TrustLineWrapper(ltx, accountID, asset);
}

ConstTrustLineWrapper
loadTrustLineWithoutRecord(AbstractLedgerTxn& ltx, AccountID const& accountID,
                           Asset const& asset)
{
    ZoneScoped;
    return ConstTrustLineWrapper(ltx, accountID, asset);
}

TrustLineWrapper
loadTrustLineIfNotNative(AbstractLedgerTxn& ltx, AccountID const& accountID,
                         Asset const& asset)
{
    ZoneScoped;
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return {};
    }
    return TrustLineWrapper(ltx, accountID, asset);
}

ConstTrustLineWrapper
loadTrustLineWithoutRecordIfNotNative(AbstractLedgerTxn& ltx,
                                      AccountID const& accountID,
                                      Asset const& asset)
{
    ZoneScoped;
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return {};
    }
    return ConstTrustLineWrapper(ltx, accountID, asset);
}

LedgerTxnEntry
loadSponsorship(AbstractLedgerTxn& ltx, AccountID const& sponsoredID)
{
    ZoneScoped;
    return ltx.load(sponsorshipKey(sponsoredID));
}

LedgerTxnEntry
loadSponsorshipCounter(AbstractLedgerTxn& ltx, AccountID const& sponsoringID)
{
    ZoneScoped;
    return ltx.load(sponsorshipCounterKey(sponsoringID));
}

LedgerTxnEntry
loadMaxSeqNumToApply(AbstractLedgerTxn& ltx, AccountID const& sourceAccount)
{
    ZoneScoped;
    return ltx.load(maxSeqNumToApplyKey(sourceAccount));
}

LedgerTxnEntry
loadPoolShareTrustLine(AbstractLedgerTxn& ltx, AccountID const& accountID,
                       PoolID const& poolID)
{
    ZoneScoped;
    return ltx.load(poolShareTrustLineKey(accountID, poolID));
}

LedgerTxnEntry
loadLiquidityPool(AbstractLedgerTxn& ltx, PoolID const& poolID)
{
    ZoneScoped;
    return ltx.load(liquidityPoolKey(poolID));
}

ConstLedgerTxnEntry
loadContractData(AbstractLedgerTxn& ltx, SCAddress const& contract,
                 SCVal const& dataKey, ContractDataDurability type)
{
    ZoneScoped;
    return ltx.loadWithoutRecord(contractDataKey(contract, dataKey, type));
}

ConstLedgerTxnEntry
loadContractCode(AbstractLedgerTxn& ltx, Hash const& hash)
{
    ZoneScoped;
    return ltx.loadWithoutRecord(contractCodeKey(hash));
}

static void
acquireOrReleaseLiabilities(AbstractLedgerTxn& ltx,
                            LedgerTxnHeader const& header,
                            LedgerTxnEntry const& offerEntry, bool isAcquire)
{
    ZoneScoped;
    // This should never happen
    auto const& offer = offerEntry.current().data.offer();
    if (offer.buying == offer.selling)
    {
        throw std::runtime_error("buying and selling same asset");
    }
    auto const& sellerID = offer.sellerID;

    auto loadAccountAndValidate = [&ltx, &sellerID]() {
        auto account = stellar::loadAccount(ltx, sellerID);
        if (!account)
        {
            throw std::runtime_error("account does not exist");
        }
        return account;
    };

    auto loadTrustAndValidate = [&ltx, &sellerID](Asset const& asset) {
        auto trust = stellar::loadTrustLine(ltx, sellerID, asset);
        if (!trust)
        {
            throw std::runtime_error("trustline does not exist");
        }
        return trust;
    };

    int64_t buyingLiabilities =
        isAcquire ? getOfferBuyingLiabilities(header, offerEntry)
                  : -getOfferBuyingLiabilities(header, offerEntry);
    if (offer.buying.type() == ASSET_TYPE_NATIVE)
    {
        auto account = loadAccountAndValidate();
        if (!addBuyingLiabilities(header, account, buyingLiabilities))
        {
            throw std::runtime_error("could not add buying liabilities");
        }
    }
    else
    {
        auto buyingTrust = loadTrustAndValidate(offer.buying);
        if (!buyingTrust.addBuyingLiabilities(header, buyingLiabilities))
        {
            throw std::runtime_error("could not add buying liabilities");
        }
    }

    int64_t sellingLiabilities =
        isAcquire ? getOfferSellingLiabilities(header, offerEntry)
                  : -getOfferSellingLiabilities(header, offerEntry);
    if (offer.selling.type() == ASSET_TYPE_NATIVE)
    {
        auto account = loadAccountAndValidate();
        if (!addSellingLiabilities(header, account, sellingLiabilities))
        {
            throw std::runtime_error("could not add selling liabilities");
        }
    }
    else
    {
        auto sellingTrust = loadTrustAndValidate(offer.selling);
        if (!sellingTrust.addSellingLiabilities(header, sellingLiabilities))
        {
            throw std::runtime_error("could not add selling liabilities");
        }
    }
}

void
acquireLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                   LedgerTxnEntry const& offer)
{
    acquireOrReleaseLiabilities(ltx, header, offer, true);
}

bool
addBalanceSkipAuthorization(LedgerTxnHeader const& header,
                            LedgerTxnEntry& entry, int64_t amount)
{
    auto& tl = entry.current().data.trustLine();
    auto newBalance = tl.balance;
    if (!stellar::addBalance(newBalance, amount, tl.limit))
    {
        return false;
    }
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_10))
    {
        if (newBalance < getSellingLiabilities(header, entry))
        {
            return false;
        }
        if (newBalance > tl.limit - getBuyingLiabilities(header, entry))
        {
            return false;
        }
    }

    tl.balance = newBalance;
    return true;
}

bool
addBalance(LedgerTxnHeader const& header, LedgerTxnEntry& entry, int64_t delta)
{
    if (entry.current().data.type() == ACCOUNT)
    {
        if (delta == 0)
        {
            return true;
        }

        auto& acc = entry.current().data.account();
        auto newBalance = acc.balance;
        if (!stellar::addBalance(newBalance, delta))
        {
            return false;
        }
        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10))
        {
            auto minBalance = getMinBalance(header.current(), acc);
            if (delta < 0 &&
                newBalance - minBalance < getSellingLiabilities(header, entry))
            {
                return false;
            }
            if (newBalance > INT64_MAX - getBuyingLiabilities(header, entry))
            {
                return false;
            }
        }

        acc.balance = newBalance;
        return true;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        if (delta == 0)
        {
            return true;
        }

        if (!checkAuthorization(header.current(), entry.current()))
        {
            return false;
        }

        return addBalanceSkipAuthorization(header, entry, delta);
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

bool
addBuyingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                     int64_t delta)
{
    int64_t buyingLiab = getBuyingLiabilities(header, entry);

    // Fast-succeed when not actually adding any liabilities
    if (delta == 0)
    {
        return true;
    }

    if (entry.current().data.type() == ACCOUNT)
    {
        auto& acc = entry.current().data.account();

        int64_t maxLiabilities = INT64_MAX - acc.balance;
        bool res = stellar::addBalance(buyingLiab, delta, maxLiabilities);
        if (res)
        {
            prepareAccountEntryExtensionV1(acc).liabilities.buying = buyingLiab;
        }
        return res;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        if (!checkAuthorization(header.current(), entry.current()))
        {
            return false;
        }

        auto& tl = entry.current().data.trustLine();
        int64_t maxLiabilities = tl.limit - tl.balance;
        bool res = stellar::addBalance(buyingLiab, delta, maxLiabilities);
        if (res)
        {
            prepareTrustLineEntryExtensionV1(tl).liabilities.buying =
                buyingLiab;
        }
        return res;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

bool
addSellingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                      int64_t delta)
{
    int64_t sellingLiab = getSellingLiabilities(header, entry);

    // Fast-succeed when not actually adding any liabilities
    if (delta == 0)
    {
        return true;
    }

    if (entry.current().data.type() == ACCOUNT)
    {
        auto& acc = entry.current().data.account();
        int64_t maxLiabilities =
            acc.balance - getMinBalance(header.current(), acc);
        if (maxLiabilities < 0)
        {
            return false;
        }

        bool res = stellar::addBalance(sellingLiab, delta, maxLiabilities);
        if (res)
        {
            prepareAccountEntryExtensionV1(acc).liabilities.selling =
                sellingLiab;
        }
        return res;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        if (!checkAuthorization(header.current(), entry.current()))
        {
            return false;
        }

        auto& tl = entry.current().data.trustLine();
        int64_t maxLiabilities = tl.balance;
        bool res = stellar::addBalance(sellingLiab, delta, maxLiabilities);
        if (res)
        {
            prepareTrustLineEntryExtensionV1(tl).liabilities.selling =
                sellingLiab;
        }
        return res;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

uint64_t
generateID(LedgerTxnHeader& header)
{
    return ++header.current().idPool;
}

int64_t
getAvailableBalance(LedgerHeader const& header, LedgerEntry const& le)
{
    int64_t avail = 0;
    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        avail = acc.balance - getMinBalance(header, acc);
    }
    else if (le.data.type() == TRUSTLINE)
    {
        // We only want to check auth starting from V10, so no need to look at
        // the return value. This will throw if unauthorized
        checkAuthorization(header, le);

        avail = le.data.trustLine().balance;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }

    if (protocolVersionStartsFrom(header.ledgerVersion, ProtocolVersion::V_10))
    {
        avail -= getSellingLiabilities(header, le);
    }
    return avail;
}

int64_t
getAvailableBalance(LedgerTxnHeader const& header, LedgerTxnEntry const& entry)
{
    return getAvailableBalance(header.current(), entry.current());
}

int64_t
getAvailableBalance(LedgerTxnHeader const& header,
                    ConstLedgerTxnEntry const& entry)
{
    return getAvailableBalance(header.current(), entry.current());
}

int64_t
getBuyingLiabilities(LedgerTxnHeader const& header, LedgerEntry const& le)
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_10))
    {
        throw std::runtime_error("Liabilities accessed before version 10");
    }

    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.buying;
    }
    else if (le.data.type() == TRUSTLINE)
    {
        auto const& tl = le.data.trustLine();
        return (tl.ext.v() == 0) ? 0 : tl.ext.v1().liabilities.buying;
    }
    throw std::runtime_error("Unknown LedgerEntry type");
}

int64_t
getBuyingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry const& entry)
{
    return getBuyingLiabilities(header, entry.current());
}

int64_t
getMaxAmountReceive(LedgerTxnHeader const& header, LedgerEntry const& le)
{
    if (le.data.type() == ACCOUNT)
    {
        int64_t maxReceive = INT64_MAX;
        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10))
        {
            auto const& acc = le.data.account();
            maxReceive -= acc.balance + getBuyingLiabilities(header, le);
        }
        return maxReceive;
    }
    if (le.data.type() == TRUSTLINE)
    {
        if (!checkAuthorization(header.current(), le))
        {
            return 0;
        }

        auto const& tl = le.data.trustLine();
        int64_t amount = tl.limit - tl.balance;
        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10))
        {
            amount -= getBuyingLiabilities(header, le);
        }
        return amount;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

int64_t
getMaxAmountReceive(LedgerTxnHeader const& header, LedgerTxnEntry const& entry)
{
    return getMaxAmountReceive(header, entry.current());
}

int64_t
getMaxAmountReceive(LedgerTxnHeader const& header,
                    ConstLedgerTxnEntry const& entry)
{
    return getMaxAmountReceive(header, entry.current());
}

int64_t
getMinBalance(LedgerHeader const& header, AccountEntry const& acc)
{
    uint32_t numSponsoring = 0;
    uint32_t numSponsored = 0;
    if (protocolVersionStartsFrom(header.ledgerVersion,
                                  ProtocolVersion::V_14) &&
        hasAccountEntryExtV2(acc))
    {
        numSponsoring = acc.ext.v1().ext.v2().numSponsoring;
        numSponsored = acc.ext.v1().ext.v2().numSponsored;
    }
    return getMinBalance(header, acc.numSubEntries, numSponsoring,
                         numSponsored);
}

int64_t
getMinBalance(LedgerHeader const& lh, uint32_t numSubentries,
              uint32_t numSponsoring, uint32_t numSponsored)
{
    if (protocolVersionIsBefore(lh.ledgerVersion, ProtocolVersion::V_14) &&
        (numSponsored != 0 || numSponsoring != 0))
    {
        throw std::runtime_error("unexpected sponsorship state");
    }

    if (protocolVersionIsBefore(lh.ledgerVersion, ProtocolVersion::V_9))
    {
        return (2 + numSubentries) * lh.baseReserve;
    }
    else
    {
        int64_t effEntries = 2LL;
        effEntries += numSubentries;
        effEntries += numSponsoring;
        effEntries -= numSponsored;
        if (effEntries < 0)
        {
            throw std::runtime_error("unexpected account state");
        }
        return effEntries * int64_t(lh.baseReserve);
    }
}

int64_t
getMinimumLimit(LedgerTxnHeader const& header, LedgerEntry const& le)
{
    auto const& tl = le.data.trustLine();
    int64_t minLimit = tl.balance;
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_10))
    {
        minLimit += getBuyingLiabilities(header, le);
    }
    return minLimit;
}

int64_t
getMinimumLimit(LedgerTxnHeader const& header, LedgerTxnEntry const& entry)
{
    return getMinimumLimit(header, entry.current());
}

int64_t
getMinimumLimit(LedgerTxnHeader const& header, ConstLedgerTxnEntry const& entry)
{
    return getMinimumLimit(header, entry.current());
}

int64_t
getOfferBuyingLiabilities(LedgerTxnHeader const& header,
                          LedgerEntry const& entry)
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_10))
    {
        throw std::runtime_error(
            "Offer liabilities calculated before version 10");
    }
    auto const& oe = entry.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX,
        RoundingType::NORMAL);
    return res.numSheepSend;
}

int64_t
getOfferBuyingLiabilities(LedgerTxnHeader const& header,
                          LedgerTxnEntry const& entry)
{
    return getOfferBuyingLiabilities(header, entry.current());
}

int64_t
getOfferSellingLiabilities(LedgerTxnHeader const& header,
                           LedgerEntry const& entry)
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_10))
    {
        throw std::runtime_error(
            "Offer liabilities calculated before version 10");
    }
    auto const& oe = entry.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX,
        RoundingType::NORMAL);
    return res.numWheatReceived;
}

int64_t
getOfferSellingLiabilities(LedgerTxnHeader const& header,
                           LedgerTxnEntry const& entry)
{
    return getOfferSellingLiabilities(header, entry.current());
}

int64_t
getSellingLiabilities(LedgerHeader const& header, LedgerEntry const& le)
{
    if (protocolVersionIsBefore(header.ledgerVersion, ProtocolVersion::V_10))
    {
        throw std::runtime_error("Liabilities accessed before version 10");
    }

    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.selling;
    }
    else if (le.data.type() == TRUSTLINE)
    {
        auto const& tl = le.data.trustLine();
        return (tl.ext.v() == 0) ? 0 : tl.ext.v1().liabilities.selling;
    }
    throw std::runtime_error("Unknown LedgerEntry type");
}

int64_t
getSellingLiabilities(LedgerTxnHeader const& header,
                      LedgerTxnEntry const& entry)
{
    return getSellingLiabilities(header.current(), entry.current());
}

SequenceNumber
getStartingSequenceNumber(uint32_t ledgerSeq)
{
    if (ledgerSeq > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
    {
        throw std::runtime_error("overflowed getStartingSequenceNumber");
    }
    return static_cast<SequenceNumber>(ledgerSeq) << 32;
}

SequenceNumber
getStartingSequenceNumber(LedgerTxnHeader const& header)
{
    return getStartingSequenceNumber(header.current().ledgerSeq);
}

SequenceNumber
getStartingSequenceNumber(LedgerHeader const& header)
{
    return getStartingSequenceNumber(header.ledgerSeq);
}

bool
isAuthorized(LedgerEntry const& le)
{
    return (le.data.trustLine().flags & AUTHORIZED_FLAG) != 0;
}

bool
isAuthorized(LedgerTxnEntry const& entry)
{
    return isAuthorized(entry.current());
}

bool
isAuthorized(ConstLedgerTxnEntry const& entry)
{
    return isAuthorized(entry.current());
}

bool
isAuthorizedToMaintainLiabilitiesUnsafe(uint32_t flags)
{
    return (flags & TRUSTLINE_AUTH_FLAGS) != 0;
}

bool
isAuthorizedToMaintainLiabilities(LedgerEntry const& le)
{
    if (le.data.trustLine().asset.type() == ASSET_TYPE_POOL_SHARE)
    {
        return true;
    }
    return isAuthorizedToMaintainLiabilitiesUnsafe(le.data.trustLine().flags);
}

bool
isAuthorizedToMaintainLiabilities(LedgerTxnEntry const& entry)
{
    return isAuthorizedToMaintainLiabilities(entry.current());
}

bool
isAuthorizedToMaintainLiabilities(ConstLedgerTxnEntry const& entry)
{
    return isAuthorizedToMaintainLiabilities(entry.current());
}

bool
isAuthRequired(ConstLedgerTxnEntry const& entry)
{
    return (entry.current().data.account().flags & AUTH_REQUIRED_FLAG) != 0;
}

bool
isClawbackEnabledOnTrustline(TrustLineEntry const& tl)
{
    return (tl.flags & TRUSTLINE_CLAWBACK_ENABLED_FLAG) != 0;
}

bool
isClawbackEnabledOnTrustline(LedgerTxnEntry const& entry)
{
    return isClawbackEnabledOnTrustline(entry.current().data.trustLine());
}

bool
isClawbackEnabledOnClaimableBalance(ClaimableBalanceEntry const& entry)
{
    return entry.ext.v() == 1 && (entry.ext.v1().flags &
                                  CLAIMABLE_BALANCE_CLAWBACK_ENABLED_FLAG) != 0;
}

bool
isClawbackEnabledOnClaimableBalance(LedgerEntry const& entry)
{
    return isClawbackEnabledOnClaimableBalance(entry.data.claimableBalance());
}

bool
isClawbackEnabledOnAccount(LedgerEntry const& entry)
{
    return (entry.data.account().flags & AUTH_CLAWBACK_ENABLED_FLAG) != 0;
}

bool
isClawbackEnabledOnAccount(LedgerTxnEntry const& entry)
{
    return isClawbackEnabledOnAccount(entry.current());
}

bool
isClawbackEnabledOnAccount(ConstLedgerTxnEntry const& entry)
{
    return isClawbackEnabledOnAccount(entry.current());
}

void
setClaimableBalanceClawbackEnabled(ClaimableBalanceEntry& cb)
{
    if (cb.ext.v() != 0)
    {
        throw std::runtime_error(
            "unexpected ClaimableBalanceEntry ext version");
    }

    cb.ext.v(1);
    cb.ext.v1().flags = CLAIMABLE_BALANCE_CLAWBACK_ENABLED_FLAG;
}

bool
isImmutableAuth(LedgerTxnEntry const& entry)
{
    return (entry.current().data.account().flags & AUTH_IMMUTABLE_FLAG) != 0;
}

static bool
isLedgerHeaderFlagSet(LedgerHeader const& header, uint32_t flag)
{
    return header.ext.v() == 1 && (header.ext.v1().flags & flag) != 0;
}

bool
isPoolDepositDisabled(LedgerHeader const& header)
{
    return isLedgerHeaderFlagSet(header, DISABLE_LIQUIDITY_POOL_DEPOSIT_FLAG);
}

bool
isPoolWithdrawalDisabled(LedgerHeader const& header)
{
    return isLedgerHeaderFlagSet(header,
                                 DISABLE_LIQUIDITY_POOL_WITHDRAWAL_FLAG);
}

bool
isPoolTradingDisabled(LedgerHeader const& header)
{
    return isLedgerHeaderFlagSet(header, DISABLE_LIQUIDITY_POOL_TRADING_FLAG);
}

void
releaseLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                   LedgerTxnEntry const& offer)
{
    acquireOrReleaseLiabilities(ltx, header, offer, false);
}

bool
trustLineFlagIsValid(uint32_t flag, uint32_t ledgerVersion)
{
    return trustLineFlagMaskCheckIsValid(flag, ledgerVersion) &&
           (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13) ||
            trustLineFlagAuthIsValid(flag));
}

bool
trustLineFlagAuthIsValid(uint32_t flag)
{
    static_assert(TRUSTLINE_AUTH_FLAGS == 3,
                  "condition only works for two flags");
    // multiple auth flags can't be set
    if ((flag & TRUSTLINE_AUTH_FLAGS) == TRUSTLINE_AUTH_FLAGS)
    {
        return false;
    }

    return true;
}

bool
trustLineFlagMaskCheckIsValid(uint32_t flag, uint32_t ledgerVersion)
{
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13))
    {
        return (flag & ~MASK_TRUSTLINE_FLAGS) == 0;
    }
    else if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_17))
    {
        return (flag & ~MASK_TRUSTLINE_FLAGS_V13) == 0;
    }
    else
    {
        return (flag & ~MASK_TRUSTLINE_FLAGS_V17) == 0;
    }
}

bool
accountFlagIsValid(uint32_t flag, uint32_t ledgerVersion)
{
    return accountFlagMaskCheckIsValid(flag, ledgerVersion) &&
           accountFlagClawbackIsValid(flag, ledgerVersion);
}

bool
accountFlagClawbackIsValid(uint32_t flag, uint32_t ledgerVersion)
{
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_17) &&
        (flag & AUTH_CLAWBACK_ENABLED_FLAG) &&
        ((flag & AUTH_REVOCABLE_FLAG) == 0))
    {
        return false;
    }

    return true;
}

bool
accountFlagMaskCheckIsValid(uint32_t flag, uint32_t ledgerVersion)
{
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_17))
    {
        return (flag & ~MASK_ACCOUNT_FLAGS) == 0;
    }

    return (flag & ~MASK_ACCOUNT_FLAGS_V17) == 0;
}

AccountID
toAccountID(MuxedAccount const& m)
{
    AccountID ret(static_cast<PublicKeyType>(m.type() & 0xff));
    switch (m.type())
    {
    case KEY_TYPE_ED25519:
        ret.ed25519() = m.ed25519();
        break;
    case KEY_TYPE_MUXED_ED25519:
        ret.ed25519() = m.med25519().ed25519;
        break;
    default:
        // this would be a bug
        abort();
    }
    return ret;
}

MuxedAccount
toMuxedAccount(AccountID const& a)
{
    MuxedAccount ret(static_cast<CryptoKeyType>(a.type()));
    switch (a.type())
    {
    case PUBLIC_KEY_TYPE_ED25519:
        ret.ed25519() = a.ed25519();
        break;
    default:
        // this would be a bug
        abort();
    }
    return ret;
}

bool
trustLineFlagIsValid(uint32_t flag, LedgerTxnHeader const& header)
{
    return trustLineFlagIsValid(flag, header.current().ledgerVersion);
}

uint64_t
getUpperBoundCloseTimeOffset(Application& app, uint64_t lastCloseTime)
{
    uint64_t currentTime = VirtualClock::to_time_t(app.getClock().system_now());

    // account for the time between closeTime and now
    uint64_t closeTimeDrift =
        currentTime <= lastCloseTime ? 0 : currentTime - lastCloseTime;

    return app.getConfig().getExpectedLedgerCloseTime().count() *
               EXPECTED_CLOSE_TIME_MULT +
           closeTimeDrift;
}

bool
hasAccountEntryExtV2(AccountEntry const& ae)
{
    return ae.ext.v() == 1 && ae.ext.v1().ext.v() == 2;
}

bool
hasAccountEntryExtV3(AccountEntry const& ae)
{
    return ae.ext.v() == 1 && ae.ext.v1().ext.v() == 2 &&
           ae.ext.v1().ext.v2().ext.v() == 3;
}

bool
hasTrustLineEntryExtV2(TrustLineEntry const& tl)
{
    return tl.ext.v() == 1 && tl.ext.v1().ext.v() == 2;
}

Asset
getAsset(AccountID const& issuer, AssetCode const& assetCode)
{
    Asset asset;
    asset.type(assetCode.type());
    if (assetCode.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        asset.alphaNum4().assetCode = assetCode.assetCode4();
        asset.alphaNum4().issuer = issuer;
    }
    else if (assetCode.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        asset.alphaNum12().assetCode = assetCode.assetCode12();
        asset.alphaNum12().issuer = issuer;
    }
    else
    {
        throw std::runtime_error("Unexpected assetCode type");
    }

    return asset;
}

bool
claimableBalanceFlagIsValid(ClaimableBalanceEntry const& cb)
{
    if (cb.ext.v() == 1)
    {
        return cb.ext.v1().flags == MASK_CLAIMABLE_BALANCE_FLAGS;
    }

    return true;
}

// The following static methods are used for authorization revocation

static void
removeOffersByAccountAndAsset(AbstractLedgerTxn& ltx, AccountID const& account,
                              Asset const& asset)
{
    LedgerTxn ltxInner(ltx);

    auto header = ltxInner.loadHeader();
    auto offers = ltxInner.loadOffersByAccountAndAsset(account, asset);
    for (auto& offer : offers)
    {
        auto const& oe = offer.current().data.offer();
        if (!(oe.sellerID == account))
        {
            throw std::runtime_error("Offer not owned by expected account");
        }
        else if (!(oe.buying == asset || oe.selling == asset))
        {
            throw std::runtime_error(
                "Offer not buying or selling expected asset");
        }

        releaseLiabilities(ltxInner, header, offer);
        auto trustAcc = stellar::loadAccount(ltxInner, account);
        removeEntryWithPossibleSponsorship(ltxInner, header, offer.current(),
                                           trustAcc);
        offer.erase();
    }
    ltxInner.commit();
}

static bool
tryGetEntrySponsor(LedgerTxnEntry& entry, AccountID& sponsor)
{
    if (entry.current().ext.v() == 1 &&
        getLedgerEntryExtensionV1(entry.current()).sponsoringID)
    {
        sponsor = *getLedgerEntryExtensionV1(entry.current()).sponsoringID;
        return true;
    }

    return false;
}

static AccountID
getTrustLineBacker(LedgerTxnEntry& trustLine)
{
    AccountID sponsor;
    if (tryGetEntrySponsor(trustLine, sponsor))
    {
        return sponsor;
    }

    return trustLine.current().data.trustLine().accountID;
}

static void
prefetchForRevokeFromPoolShareTrustLines(
    AbstractLedgerTxn& ltx, AccountID const& accountID,
    std::vector<LedgerTxnEntry>& poolShareTrustLines)
{
    // first prefetch the liquidity pools and pool share trustline sponsors
    UnorderedSet<LedgerKey> keys;
    for (auto& trustLine : poolShareTrustLines)
    {
        keys.emplace(liquidityPoolKey(
            trustLine.current().data.trustLine().asset.liquidityPoolID()));

        AccountID sponsor;
        if (tryGetEntrySponsor(trustLine, sponsor))
        {
            keys.emplace(accountKey(sponsor));
        }
    }
    ltx.prefetchClassic(keys);

    // now prefetch the asset trustlines
    keys.clear();
    for (auto const& trustLine : poolShareTrustLines)
    {
        // prefetching shouldn't affect the protocol, so use loadWithoutRecord
        // to not touch lastModified
        auto pool = ltx.loadWithoutRecord(liquidityPoolKey(
            trustLine.current().data.trustLine().asset.liquidityPoolID()));

        auto const& params =
            pool.current().data.liquidityPool().body.constantProduct().params;

        if (params.assetA.type() != ASSET_TYPE_NATIVE &&
            !isIssuer(accountID, params.assetA))
        {
            keys.emplace(trustlineKey(accountID, params.assetA));
        }

        if (params.assetB.type() != ASSET_TYPE_NATIVE &&
            !isIssuer(accountID, params.assetB))
        {
            keys.emplace(trustlineKey(accountID, params.assetB));
        }
    }
    ltx.prefetchClassic(keys);
}

static ClaimableBalanceID
getRevokeID(AccountID const& txSourceID, SequenceNumber txSeqNum,
            uint32_t opIndex, PoolID const& poolID, Asset const& asset)
{
    HashIDPreimage hashPreimage;
    hashPreimage.type(ENVELOPE_TYPE_POOL_REVOKE_OP_ID);
    hashPreimage.revokeID().sourceAccount = txSourceID;
    hashPreimage.revokeID().seqNum = txSeqNum;
    hashPreimage.revokeID().opNum = opIndex;
    hashPreimage.revokeID().liquidityPoolID = poolID;
    hashPreimage.revokeID().asset = asset;

    ClaimableBalanceID balanceID;
    balanceID.type(CLAIMABLE_BALANCE_ID_TYPE_V0);
    balanceID.v0() = xdrSha256(hashPreimage);

    return balanceID;
}

static Claimant
makeUnconditionalClaimant(AccountID const& accountID)
{
    Claimant c;
    c.v0().destination = accountID;
    c.v0().predicate.type(CLAIM_PREDICATE_UNCONDITIONAL);

    return c;
}

static std::vector<LedgerKey>
prefetchPoolShareTrustLinesByAccountAndGetKeys(AbstractLedgerTxn& ltx,
                                               AccountID const& accountID,
                                               Asset const& asset)
{
    // always rolls back
    LedgerTxn ltxInner(ltx);

    // this will get the pool share trustlines into the mEntryCache
    auto poolShareTrustLines =
        ltxInner.loadPoolShareTrustLinesByAccountAndAsset(accountID, asset);

    if (poolShareTrustLines.empty())
    {
        return {};
    }

    prefetchForRevokeFromPoolShareTrustLines(ltxInner, accountID,
                                             poolShareTrustLines);

    std::vector<LedgerKey> poolTLKeys;
    for (auto const& poolShareTrustLine : poolShareTrustLines)
    {
        poolTLKeys.emplace_back(LedgerEntryKey(poolShareTrustLine.current()));
    }

    return poolTLKeys;
}

RemoveResult
removeOffersAndPoolShareTrustLines(AbstractLedgerTxn& ltx,
                                   AccountID const& accountID,
                                   Asset const& asset,
                                   AccountID const& txSourceID,
                                   SequenceNumber txSeqNum, uint32_t opIndex)
{
    removeOffersByAccountAndAsset(ltx, accountID, asset);

    LedgerTxn ltxInner(ltx);

    if (protocolVersionIsBefore(ltxInner.loadHeader().current().ledgerVersion,
                                ProtocolVersion::V_18))
    {
        return RemoveResult::SUCCESS;
    }

    // Get the keys and load the pool share trustlines individually below so we
    // can use nested LedgerTxns.
    auto poolTLKeys = prefetchPoolShareTrustLinesByAccountAndGetKeys(
        ltxInner, accountID, asset);
    if (poolTLKeys.empty())
    {
        return RemoveResult::SUCCESS;
    }

    for (auto const& poolTLKey : poolTLKeys)
    {
        auto poolShareTrustLine = loadPoolShareTrustLine(
            ltxInner, accountID, poolTLKey.trustLine().asset.liquidityPoolID());

        // use a lambda so we don't hold a reference to the internals of
        // TrustLineEntry
        auto poolTL = [&]() -> TrustLineEntry const& {
            return poolShareTrustLine.current().data.trustLine();
        };

        auto poolID = poolTL().asset.liquidityPoolID();
        auto balance = poolTL().balance;
        auto cbSponsoringAccID = getTrustLineBacker(poolShareTrustLine);

        // release reserves and delete the pool share trustline
        {
            auto trustAcc = stellar::loadAccount(ltxInner, accountID);
            removeEntryWithPossibleSponsorship(ltxInner, ltxInner.loadHeader(),
                                               poolShareTrustLine.current(),
                                               trustAcc);

            // using poolTL() after this will throw an exception
            poolShareTrustLine.erase();
        }

        auto redeemIntoClaimableBalance =
            [&ltxInner, &txSourceID, txSeqNum, opIndex, &poolID, &accountID,
             &cbSponsoringAccID](Asset const& assetInPool,
                                 int64_t amount) -> RemoveResult {
            // if the amount is 0 or the claimant is the issuer, then we don't
            // create the claimable balance
            if (isIssuer(accountID, assetInPool) || amount == 0)
            {
                return RemoveResult::SUCCESS;
            }

            // create a claimable balance for the asset in the pool
            LedgerEntry claimableBalance;
            claimableBalance.data.type(CLAIMABLE_BALANCE);

            auto& claimableBalanceEntry =
                claimableBalance.data.claimableBalance();

            claimableBalanceEntry.balanceID =
                getRevokeID(txSourceID, txSeqNum, opIndex, poolID, assetInPool);
            claimableBalanceEntry.amount = amount;
            claimableBalanceEntry.asset = assetInPool;
            claimableBalanceEntry.claimants = {
                makeUnconditionalClaimant(accountID)};

            // if this asset isn't native
            // 1. set clawback if it's set on the trustline
            // 2. decrement liquidityPoolUseCount on the asset trustline
            if (assetInPool.type() != ASSET_TYPE_NATIVE)
            {
                // asset trustline must exist because the pool share trustline
                // exists, and this lambda returns early if the issuer is the
                // claimant
                auto assetTrustLine =
                    loadTrustLine(ltxInner, accountID, assetInPool);
                if (assetTrustLine.isClawbackEnabled())
                {
                    setClaimableBalanceClawbackEnabled(claimableBalanceEntry);
                }
            }

            // The account that sponsored the deleted pool share trustline will
            // be used for the claimable balances. If that account is currently
            // in a sponsorship sandwich, that sponsoring account will instead
            // sponsor the claimable balance.
            if (loadSponsorship(ltxInner, cbSponsoringAccID))
            {
                auto cbSponsoringLtxAcc =
                    stellar::loadAccount(ltxInner, cbSponsoringAccID);
                switch (createEntryWithPossibleSponsorship(
                    ltxInner, ltxInner.loadHeader(), claimableBalance,
                    cbSponsoringLtxAcc))
                {
                case SponsorshipResult::SUCCESS:
                    break;
                // LOW_RESERVE and TOO_MANY_SPONSORING are possible because the
                // sponsoring account in the sandwich was not the account that
                // released the pool share trustline, so there might not be
                // available reserves or sponsoring space
                case SponsorshipResult::LOW_RESERVE:
                    return RemoveResult::LOW_RESERVE;
                case SponsorshipResult::TOO_MANY_SPONSORING:
                    return RemoveResult::TOO_MANY_SPONSORING;
                case SponsorshipResult::TOO_MANY_SPONSORED:
                    // This is impossible because there's no sponsored account.
                    // Fall through and throw.
                case SponsorshipResult::TOO_MANY_SUBENTRIES:
                    // This is impossible because claimable balances don't use
                    // subentries. Fall through and throw.
                default:
                    throw std::runtime_error("Unexpected result from "
                                             "canEstablishEntrySponsorship");
                }
            }
            else
            {
                // not in a sponsorship sandwich
                // we don't use createEntryWithPossibleSponsorship here since
                // it can return LOW_RESERVE if the base reserve
                // was increased after the pool share trustline was created. We
                // are allowing this claimable balance to take the reserve that
                // the pool share trustline took, even if this account is below
                // the minimum balance.
                auto cbSponsoringLtxAcc =
                    stellar::loadAccount(ltxInner, cbSponsoringAccID);

                uint32_t mult = computeMultiplier(claimableBalance);
                if (getNumSponsoring(cbSponsoringLtxAcc.current()) >
                    UINT32_MAX - mult)
                {
                    throw std::runtime_error(
                        "no numSponsoring available for revoke");
                }

                establishEntrySponsorship(
                    claimableBalance, cbSponsoringLtxAcc.current(), nullptr);
            }

            ltxInner.create(claimableBalance);
            return RemoveResult::SUCCESS;
        };

        auto pool = loadLiquidityPool(ltxInner, poolID);
        // use a lambda so we don't hold a reference to the
        // LiquidityPoolEntry
        auto constantProduct = [&]() -> auto&
        {
            return pool.current().data.liquidityPool().body.constantProduct();
        };

        if (balance != 0)
        {
            auto amountA = getPoolWithdrawalAmount(
                balance, constantProduct().totalPoolShares,
                constantProduct().reserveA);

            if (auto res = redeemIntoClaimableBalance(
                    constantProduct().params.assetA, amountA);
                res != RemoveResult::SUCCESS)
            {
                return res;
            }

            auto amountB = getPoolWithdrawalAmount(
                balance, constantProduct().totalPoolShares,
                constantProduct().reserveB);

            if (auto res = redeemIntoClaimableBalance(
                    constantProduct().params.assetB, amountB);
                res != RemoveResult::SUCCESS)
            {
                return res;
            }

            constantProduct().totalPoolShares -= balance;
            constantProduct().reserveA -= amountA;
            constantProduct().reserveB -= amountB;
        }

        // decrementLiquidityPoolUseCount will create a nested LedgerTxn and
        // deactivate all loaded entries, so copy the assets here
        auto const assetA = constantProduct().params.assetA;
        auto const assetB = constantProduct().params.assetB;

        decrementLiquidityPoolUseCount(ltxInner, assetA, accountID);
        decrementLiquidityPoolUseCount(ltxInner, assetB, accountID);

        // pool was deactivated in decrementLiquidityPoolUseCount
        pool = loadLiquidityPool(ltxInner, poolID);
        decrementPoolSharesTrustLineCount(pool);
    }

    ltxInner.commit();
    return RemoveResult::SUCCESS;
}

void
decrementPoolSharesTrustLineCount(LedgerTxnEntry& liquidityPool)
{
    auto poolTLCount = --liquidityPool.current()
                             .data.liquidityPool()
                             .body.constantProduct()
                             .poolSharesTrustLineCount;
    if (poolTLCount == 0)
    {
        liquidityPool.erase();
    }
    else if (poolTLCount < 0)
    {
        throw std::runtime_error("poolSharesTrustLineCount is negative");
    }
}

void
decrementLiquidityPoolUseCount(AbstractLedgerTxn& ltx, Asset const& asset,
                               AccountID const& accountID)
{
    LedgerTxn ltxInner(ltx);
    if (!isIssuer(accountID, asset) && asset.type() != ASSET_TYPE_NATIVE)
    {
        auto assetTrustLine = ltxInner.load(trustlineKey(accountID, asset));
        if (!assetTrustLine)
        {
            throw std::runtime_error("asset trustline is missing");
        }

        if (--getTrustLineEntryExtensionV2(
                  assetTrustLine.current().data.trustLine())
                  .liquidityPoolUseCount < 0)
        {
            throw std::runtime_error("liquidityPoolUseCount is negative");
        }
    }
    ltxInner.commit();
}

template <typename T>
T
assetConversionHelper(Asset const& asset)
{
    T otherAsset;
    otherAsset.type(asset.type());

    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        otherAsset.alphaNum4() = asset.alphaNum4();
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        otherAsset.alphaNum12() = asset.alphaNum12();
        break;
    case stellar::ASSET_TYPE_POOL_SHARE:
        throw std::runtime_error("Asset can't have type ASSET_TYPE_POOL_SHARE");
    default:
        throw std::runtime_error("Unknown asset type");
    }

    return otherAsset;
}

TrustLineAsset
assetToTrustLineAsset(Asset const& asset)
{
    return assetConversionHelper<TrustLineAsset>(asset);
}

ChangeTrustAsset
assetToChangeTrustAsset(Asset const& asset)
{
    return assetConversionHelper<ChangeTrustAsset>(asset);
}

TrustLineAsset
changeTrustAssetToTrustLineAsset(ChangeTrustAsset const& ctAsset)
{
    TrustLineAsset tlAsset;
    tlAsset.type(ctAsset.type());

    switch (ctAsset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        tlAsset.alphaNum4() = ctAsset.alphaNum4();
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        tlAsset.alphaNum12() = ctAsset.alphaNum12();
        break;
    case stellar::ASSET_TYPE_POOL_SHARE:
        tlAsset.liquidityPoolID() = xdrSha256(ctAsset.liquidityPool());
        break;
    default:
        throw std::runtime_error("Unknown asset type");
    }

    return tlAsset;
}

int64_t
getPoolWithdrawalAmount(int64_t amountPoolShares, int64_t totalPoolShares,
                        int64_t reserve)
{
    if (amountPoolShares > totalPoolShares)
    {
        throw std::runtime_error("Invalid amountPoolShares");
    }

    return bigDivideOrThrow(amountPoolShares, reserve, totalPoolShares,
                            ROUND_DOWN);
}

void
maybeUpdateAccountOnLedgerSeqUpdate(LedgerTxnHeader const& header,
                                    LedgerTxnEntry& account)
{
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        auto& v3 =
            prepareAccountEntryExtensionV3(account.current().data.account());
        v3.seqLedger = header.current().ledgerSeq;
        v3.seqTime = header.current().scpValue.closeTime;
    }
}

int64_t
getMinInclusionFee(TransactionFrameBase const& tx, LedgerHeader const& header,
                   std::optional<int64_t> baseFee)
{
    int64_t effectiveBaseFee = header.baseFee;
    if (baseFee)
    {
        effectiveBaseFee = std::max(effectiveBaseFee, *baseFee);
    }
    return effectiveBaseFee * std::max<int64_t>(1, tx.getNumOperations());
}

bool
validateContractLedgerEntry(LedgerKey const& lk, size_t entrySize,
                            SorobanNetworkConfig const& config,
                            Config const& appConfig,
                            TransactionFrame const& parentTx,
                            SorobanTxData& sorobanData)
{
    // check contract code size limit
    if (lk.type() == CONTRACT_CODE && config.maxContractSizeBytes() < entrySize)
    {
        sorobanData.pushApplyTimeDiagnosticError(
            appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "Wasm size exceeds network config maximum contract size",
            {makeU64SCVal(entrySize),
             makeU64SCVal(config.maxContractSizeBytes())});
        return false;
    }
    // check contract data entry size limit
    if (lk.type() == CONTRACT_DATA &&
        config.maxContractDataEntrySizeBytes() < entrySize)
    {
        sorobanData.pushApplyTimeDiagnosticError(
            appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "ContractData size exceeds network config maximum size",
            {makeU64SCVal(entrySize),
             makeU64SCVal(config.maxContractDataEntrySizeBytes())});
        return false;
    }
    return true;
}

LumenContractInfo
getLumenContractInfo(Hash const& networkID)
{
    // Calculate contractID
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID);
    preImage.contractID().networkID = networkID;

    Asset native;
    native.type(ASSET_TYPE_NATIVE);
    preImage.contractID().contractIDPreimage.type(
        CONTRACT_ID_PREIMAGE_FROM_ASSET);
    preImage.contractID().contractIDPreimage.fromAsset() = native;

    auto lumenContractID = xdrSha256(preImage);

    // Calculate SCVal for balance key
    SCVal balanceSymbol(SCV_SYMBOL);
    balanceSymbol.sym() = "Balance";

    // Calculate SCVal for amount key
    SCVal amountSymbol(SCV_SYMBOL);
    amountSymbol.sym() = "amount";

    return {lumenContractID, balanceSymbol, amountSymbol};
}

SCVal
makeSymbolSCVal(std::string&& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(std::move(str));
    return val;
}

SCVal
makeSymbolSCVal(std::string const& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(str);
    return val;
}

SCVal
makeStringSCVal(std::string&& str)
{
    SCVal val(SCV_STRING);
    val.str().assign(std::move(str));
    return val;
}

SCVal
makeU64SCVal(uint64_t u)
{
    SCVal val(SCV_U64);
    val.u64() = u;
    return val;
}

SCVal
makeAddressSCVal(SCAddress const& address)
{
    SCVal val(SCV_ADDRESS);
    val.address() = address;
    return val;
}

namespace detail
{
struct MuxChecker
{
    bool mHasMuxedAccount{false};

    void
    operator()(stellar::MuxedAccount const& t)
    {
        // checks if this is a multiplexed account,
        // such as KEY_TYPE_MUXED_ED25519
        if ((t.type() & 0x100) != 0)
        {
            mHasMuxedAccount = true;
        }
    }

    template <typename T>
    std::enable_if_t<(xdr::xdr_traits<T>::is_container ||
                      xdr::xdr_traits<T>::is_class)>
    operator()(T const& t)
    {
        if (!mHasMuxedAccount)
        {
            xdr::xdr_traits<T>::save(*this, t);
        }
    }

    template <typename T>
    std::enable_if_t<!(xdr::xdr_traits<T>::is_container ||
                       xdr::xdr_traits<T>::is_class)>
    operator()(T const& t)
    {
    }
};
} // namespace detail

bool
hasMuxedAccount(TransactionEnvelope const& e)
{
    detail::MuxChecker c;
    c(e);
    return c.mHasMuxedAccount;
}

bool
isTransactionXDRValidForProtocol(uint32_t currProtocol, Config const& cfg,
                                 TransactionEnvelope const& envelope)
{
    uint32_t maxProtocol = cfg.CURRENT_LEDGER_PROTOCOL_VERSION;
    // If we could parse the XDR when ledger is using the maximum supported
    // protocol version, then XDR has to be valid.
    // This check also is pointless before protocol 21 as Soroban environment
    // doesn't support XDR versions before 21.
    if (maxProtocol == currProtocol ||
        protocolVersionIsBefore(currProtocol, ProtocolVersion::V_21))
    {
        return true;
    }
    auto cxxBuf = CxxBuf{
        std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(envelope))};
    return rust_bridge::can_parse_transaction(maxProtocol, currProtocol, cxxBuf,
                                              xdr::marshaling_stack_limit);
}

ClaimAtom
makeClaimAtom(uint32_t ledgerVersion, AccountID const& accountID,
              int64_t offerID, Asset const& wheat, int64_t numWheatReceived,
              Asset const& sheep, int64_t numSheepSend)
{
    ClaimAtom atom;
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_18))
    {
        atom.type(CLAIM_ATOM_TYPE_V0);
        atom.v0() = ClaimOfferAtomV0(accountID.ed25519(), offerID, wheat,
                                     numWheatReceived, sheep, numSheepSend);
    }
    else
    {
        atom.type(CLAIM_ATOM_TYPE_ORDER_BOOK);
        atom.orderBook() = ClaimOfferAtom(
            accountID, offerID, wheat, numWheatReceived, sheep, numSheepSend);
    }
    return atom;
}
} // namespace stellar
