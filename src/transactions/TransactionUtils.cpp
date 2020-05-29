// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionUtils.h"
#include "crypto/SecretKey.h"
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

bool
checkAuthorization(LedgerTxnHeader const& header, LedgerEntry const& entry)
{
    if (header.current().ledgerVersion < 10)
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
            auto minBalance = getMinBalance(header, acc.numSubEntries);
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

        if (!checkAuthorization(header, entry.current()))
        {
            return false;
        }

        auto& tl = entry.current().data.trustLine();
        auto newBalance = tl.balance;
        if (!stellar::addBalance(newBalance, delta, tl.limit))
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
            if (acc.ext.v() == 0)
            {
                acc.ext.v(1);
                acc.ext.v1().liabilities = Liabilities{0, 0};
            }
            acc.ext.v1().liabilities.buying = buyingLiab;
        }
        return res;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        if (!checkAuthorization(header, entry.current()))
        {
            return false;
        }

        auto& tl = entry.current().data.trustLine();
        int64_t maxLiabilities = tl.limit - tl.balance;
        bool res = stellar::addBalance(buyingLiab, delta, maxLiabilities);
        if (res)
        {
            if (tl.ext.v() == 0)
            {
                tl.ext.v(1);
                tl.ext.v1().liabilities = Liabilities{0, 0};
            }
            tl.ext.v1().liabilities.buying = buyingLiab;
        }
        return res;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

AddSubentryResult
addNumEntries(LedgerTxnHeader const& header, LedgerTxnEntry& entry, int count)
{
    auto& acc = entry.current().data.account();
    int newEntriesCount = unsignedToSigned(acc.numSubEntries) + count;
    if (newEntriesCount < 0)
    {
        throw std::runtime_error("invalid account state");
    }
    if (header.current().ledgerVersion >=
            FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS &&
        count > 0 && newEntriesCount > ACCOUNT_SUBENTRY_LIMIT)
    {
        return AddSubentryResult::TOO_MANY_SUBENTRIES;
    }

    int64_t effMinBalance = getMinBalance(header, newEntriesCount);
    if (header.current().ledgerVersion >= 10)
    {
        effMinBalance += getSellingLiabilities(header, entry);
    }

    // only check minBalance when attempting to add subEntries
    if (count > 0 && acc.balance < effMinBalance)
    {
        // balance too low
        return AddSubentryResult::LOW_RESERVE;
    }
    acc.numSubEntries = newEntriesCount;
    return AddSubentryResult::SUCCESS;
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
            acc.balance - getMinBalance(header, acc.numSubEntries);
        if (maxLiabilities < 0)
        {
            return false;
        }

        bool res = stellar::addBalance(sellingLiab, delta, maxLiabilities);
        if (res)
        {
            if (acc.ext.v() == 0)
            {
                acc.ext.v(1);
                acc.ext.v1().liabilities = Liabilities{0, 0};
            }
            acc.ext.v1().liabilities.selling = sellingLiab;
        }
        return res;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        if (!checkAuthorization(header, entry.current()))
        {
            return false;
        }

        auto& tl = entry.current().data.trustLine();
        int64_t maxLiabilities = tl.balance;
        bool res = stellar::addBalance(sellingLiab, delta, maxLiabilities);
        if (res)
        {
            if (tl.ext.v() == 0)
            {
                tl.ext.v(1);
                tl.ext.v1().liabilities = Liabilities{0, 0};
            }
            tl.ext.v1().liabilities.selling = sellingLiab;
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
getAvailableBalance(LedgerTxnHeader const& header, LedgerEntry const& le)
{
    int64_t avail = 0;
    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        avail = acc.balance - getMinBalance(header, acc.numSubEntries);
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

    if (header.current().ledgerVersion >= 10)
    {
        avail -= getSellingLiabilities(header, le);
    }
    return avail;
}

int64_t
getAvailableBalance(LedgerTxnHeader const& header, LedgerTxnEntry const& entry)
{
    return getAvailableBalance(header, entry.current());
}

int64_t
getAvailableBalance(LedgerTxnHeader const& header,
                    ConstLedgerTxnEntry const& entry)
{
    return getAvailableBalance(header, entry.current());
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
        if (!checkAuthorization(header, le))
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
getMinBalance(LedgerTxnHeader const& header, uint32_t ownerCount)
{
    auto const& lh = header.current();
    if (lh.ledgerVersion <= 8)
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2LL + ownerCount) * int64_t(lh.baseReserve);
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
getSellingLiabilities(LedgerTxnHeader const& header, LedgerEntry const& le)
{
    if (header.current().ledgerVersion < 10)
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
    return getSellingLiabilities(header, entry.current());
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
isImmutableAuth(LedgerTxnEntry const& entry)
{
    return (entry.current().data.account().flags & AUTH_IMMUTABLE_FLAG) != 0;
}

void
normalizeSigners(LedgerTxnEntry& entry)
{
    auto& acc = entry.current().data.account();
    normalizeSigners(acc);
}

void
normalizeSigners(AccountEntry& acc)
{
    std::sort(
        acc.signers.begin(), acc.signers.end(),
        [](Signer const& s1, Signer const& s2) { return s1.key < s2.key; });
}

void
releaseLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                   LedgerTxnEntry const& offer)
{
    acquireOrReleaseLiabilities(ltx, header, offer, false);
}

void
setAuthorized(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
              uint32_t authorized)
{
    if (!trustLineFlagIsValid(authorized, header))
    {
        throw std::runtime_error("trying to set invalid trust line flag");
    }
    auto& tl = entry.current().data.trustLine();
    tl.flags = authorized;
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
        return (flag & ~MASK_TRUSTLINE_FLAGS_V13) == 0 &&
               (flag & invalidAuthCombo) != invalidAuthCombo;
    }
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
