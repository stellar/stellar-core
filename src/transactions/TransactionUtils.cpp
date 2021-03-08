// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionUtils.h"
#include "crypto/SecretKey.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/OfferExchange.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include <Tracy.hpp>

namespace stellar
{

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

AccountEntryExtensionV2&
getAccountEntryExtensionV2(AccountEntry& ae)
{
    if (ae.ext.v() != 1 || ae.ext.v1().ext.v() != 2)
    {
        throw std::runtime_error("expected AccountEntry extension V2");
    }
    return ae.ext.v1().ext.v2();
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
    if (header.ledgerVersion < 10)
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

InternalLedgerKey
sponsorshipKey(AccountID const& sponsoredID)
{
    InternalLedgerKey gkey(InternalLedgerEntryType::SPONSORSHIP);
    gkey.sponsorshipKey().sponsoredID = sponsoredID;
    return gkey;
}

InternalLedgerKey
sponsorshipCounterKey(AccountID const& sponsoringID)
{
    InternalLedgerKey gkey(InternalLedgerEntryType::SPONSORSHIP_COUNTER);
    gkey.sponsorshipCounterKey().sponsoringID = sponsoringID;
    return gkey;
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
    return ltx.load(sponsorshipKey(sponsoredID));
}

LedgerTxnEntry
loadSponsorshipCounter(AbstractLedgerTxn& ltx, AccountID const& sponsoringID)
{
    return ltx.load(sponsorshipCounterKey(sponsoringID));
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
    if (header.current().ledgerVersion >= 10)
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
        if (header.current().ledgerVersion >= 10)
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

    if (header.ledgerVersion >= 10)
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
    if (header.current().ledgerVersion < 10)
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
        if (header.current().ledgerVersion >= 10)
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
        if (header.current().ledgerVersion >= 10)
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
    if (header.ledgerVersion >= 14 && hasAccountEntryExtV2(acc))
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
    if (lh.ledgerVersion < 14 && (numSponsored != 0 || numSponsoring != 0))
    {
        throw std::runtime_error("unexpected sponsorship state");
    }

    if (lh.ledgerVersion <= 8)
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
    if (header.current().ledgerVersion >= 10)
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
    if (header.current().ledgerVersion < 10)
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
    if (header.current().ledgerVersion < 10)
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
    if (header.ledgerVersion < 10)
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

uint64_t
getStartingSequenceNumber(uint32_t ledgerSeq)
{
    return static_cast<uint64_t>(ledgerSeq) << 32;
}

uint64_t
getStartingSequenceNumber(LedgerTxnHeader const& header)
{
    return getStartingSequenceNumber(header.current().ledgerSeq);
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
isAuthorizedToMaintainLiabilities(LedgerEntry const& le)
{
    return isAuthorized(le) || (le.data.trustLine().flags &
                                AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG) != 0;
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
isClawbackEnabledOnTrustline(LedgerTxnEntry const& entry)
{
    return (entry.current().data.trustLine().flags &
            TRUSTLINE_CLAWBACK_ENABLED_FLAG) != 0;
}

bool
isClawbackEnabledOnClaimableBalance(LedgerEntry const& entry)
{
    return entry.data.claimableBalance().ext.v() == 1 &&
           (entry.data.claimableBalance().ext.v1().flags &
            CLAIMABLE_BALANCE_CLAWBACK_ENABLED_FLAG) != 0;
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

bool
isImmutableAuth(LedgerTxnEntry const& entry)
{
    return (entry.current().data.account().flags & AUTH_IMMUTABLE_FLAG) != 0;
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
    if (ledgerVersion < 13)
    {
        return (flag & ~MASK_TRUSTLINE_FLAGS) == 0;
    }
    else
    {
        uint32_t invalidAuthCombo =
            AUTHORIZED_FLAG | AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG;
        if ((flag & invalidAuthCombo) == invalidAuthCombo)
        {
            return false;
        }

        if (ledgerVersion < 16)
        {
            return (flag & ~MASK_TRUSTLINE_FLAGS_V13) == 0;
        }
        else
        {
            return (flag & ~MASK_TRUSTLINE_FLAGS_V16) == 0;
        }
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
    if (ledgerVersion >= 16 && (flag & AUTH_CLAWBACK_ENABLED_FLAG) &&
        ((flag & AUTH_REVOCABLE_FLAG) == 0))
    {
        return false;
    }

    return true;
}

bool
accountFlagMaskCheckIsValid(uint32_t flag, uint32_t ledgerVersion)
{
    if (ledgerVersion < 16)
    {
        return (flag & ~MASK_ACCOUNT_FLAGS) == 0;
    }

    return (flag & ~MASK_ACCOUNT_FLAGS_V16) == 0;
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
} // namespace stellar
