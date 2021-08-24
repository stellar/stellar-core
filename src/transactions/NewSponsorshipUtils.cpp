// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/NewSponsorshipUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{
namespace SponsorshipUtils
{
uint32_t
getNumSponsored(LedgerEntry const& le)
{
    auto const& ae = le.data.account();
    if (hasAccountEntryExtV2(ae))
    {
        return ae.ext.v1().ext.v2().numSponsored;
    }
    return 0;
}

uint32_t
getNumSponsoring(LedgerEntry const& le)
{
    auto const& ae = le.data.account();
    if (hasAccountEntryExtV2(ae))
    {
        return ae.ext.v1().ext.v2().numSponsoring;
    }
    return 0;
}

std::unique_ptr<Sponsorable>
makeSponsorable(LedgerKey const& key)
{
    switch (key.type())
    {
    case ACCOUNT:
    case TRUSTLINE:
    case OFFER:
    case DATA:
        return std::make_unique<OwnedEntrySponsorable>(key);
    case CLAIMABLE_BALANCE:
        return std::make_unique<UnownedEntrySponsorable>(
            key.claimableBalance().balanceID);
    default:
        throw std::runtime_error("unknown ledger entry type");
    }
}

static void
throwBeforeSponsorshipsEnabled(LedgerHeader const& lh)
{
    if (lh.ledgerVersion < 14)
    {
        throw std::runtime_error("sponsorship before version 14");
    }
}

static void
throwIfSponsored(LedgerEntry const& le)
{
    if (le.ext.v() == 1 && le.ext.v1().sponsoringID)
    {
        throw std::runtime_error("entry is sponsored but shouldn't be");
    }
}

static void
throwIfNotSponsored(LedgerEntry const& le)
{
    if (le.ext.v() == 0 || !le.ext.v1().sponsoringID)
    {
        throw std::runtime_error("entry isn't sponsored but should be");
    }
}

static bool
addNumSponsoring(LedgerHeader const& lh, LedgerEntry& sponsoring, int64_t mult,
                 SponsorshipResult& res)
{
    int64_t reserve = (int64_t)mult * (int64_t)lh.baseReserve;
    if (getAvailableBalance(lh, sponsoring) < reserve)
    {
        res = SponsorshipResult::LOW_RESERVE;
        return false;
    }
    if (getNumSponsoring(sponsoring) > UINT32_MAX - mult)
    {
        res = SponsorshipResult::TOO_MANY_SPONSORING;
        return false;
    }

    prepareAccountEntryExtensionV2(sponsoring.data.account()).numSponsoring +=
        mult;
    return true;
}

static bool
addNumSponsored(LedgerHeader const& lh, LedgerEntry& owner, int64_t mult,
                SponsorshipResult& res)
{
    if (getNumSponsored(owner) > UINT32_MAX - mult)
    {
        res = SponsorshipResult::TOO_MANY_SPONSORED;
        return false;
    }

    prepareAccountEntryExtensionV2(owner.data.account()).numSponsored += mult;
    return true;
}

static void
removeNumSponsoring(LedgerEntry& sponsoring, uint32_t mult)
{
    if (getNumSponsoring(sponsoring) < mult)
    {
        throw std::runtime_error("insufficient numSponsoring");
    }
    getAccountEntryExtensionV2(sponsoring.data.account()).numSponsoring -= mult;
}

static void
removeNumSponsored(LedgerEntry& owner, uint32_t mult)
{
    if (getNumSponsored(owner) < mult)
    {
        throw std::runtime_error("insufficient numSponsored");
    }
    getAccountEntryExtensionV2(owner.data.account()).numSponsored -= mult;
}

////////////////////////////////////////////////////////////////////////////////
//
// EntrySponsorable implementation
//
////////////////////////////////////////////////////////////////////////////////
template <typename Base>
SponsorshipResult
EntrySponsorable<Base>::establishSponsorshipHelper(
    AbstractLedgerTxn& ltxOuter, AccountID const& sponsoringID)
{
    LedgerTxn ltx(ltxOuter);

    auto ltxh = ltx.loadHeader();
    throwBeforeSponsorshipsEnabled(ltxh.current());

    throwIfSponsored(load(ltx).current());
    uint32_t const mult = getMult(ltx);

    SponsorshipResult res = SponsorshipResult::SUCCESS;
    if (!addNumSponsoring(ltxh.current(),
                          loadAccount(ltx, sponsoringID).current(), mult, res))
    {
        return res;
    }
    if (auto ltxeOwner = loadOwner(ltx))
    {
        if (!addNumSponsored(ltxh.current(), ltxeOwner.current(), mult, res))
        {
            return res;
        }
    }
    prepareLedgerEntryExtensionV1(load(ltx).current()).sponsoringID.activate() =
        sponsoringID;

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

template <typename Base>
SponsorshipResult
EntrySponsorable<Base>::removeSponsorshipHelper(AbstractLedgerTxn& ltxOuter,
                                                bool checkReserve)
{
    LedgerTxn ltx(ltxOuter);

    auto ltxh = ltx.loadHeader();
    throwBeforeSponsorshipsEnabled(ltxh.current());

    throwIfNotSponsored(load(ltx).current());
    uint32_t const mult = getMult(ltx);

    removeNumSponsoring(loadSponsoring(ltx).current(), mult);
    if (auto ltxeOwner = loadOwner(ltx))
    {
        removeNumSponsored(ltxeOwner.current(), mult);

        // It is permitted to be beneath the reserve when erasing entries
        if (checkReserve &&
            getAvailableBalance(ltxh.current(), ltxeOwner.current()) < 0)
        {
            return SponsorshipResult::LOW_RESERVE;
        }
    }
    load(ltx).current().ext.v1().sponsoringID.reset();

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

template <typename Base>
SponsorshipResult
EntrySponsorable<Base>::transferSponsorshipHelper(
    AbstractLedgerTxn& ltxOuter, AccountID const& newSponsoringID)
{
    LedgerTxn ltx(ltxOuter);

    auto ltxh = ltx.loadHeader();
    throwBeforeSponsorshipsEnabled(ltxh.current());

    throwIfNotSponsored(load(ltx).current());
    uint32_t const mult = getMult(ltx);

    SponsorshipResult res = SponsorshipResult::SUCCESS;
    removeNumSponsoring(loadSponsoring(ltx).current(), mult);
    if (!addNumSponsoring(ltxh.current(),
                          loadAccount(ltx, newSponsoringID).current(), mult,
                          res))
    {
        return res;
    }
    load(ltx).current().ext.v1().sponsoringID.activate() = newSponsoringID;

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
//
// OwnedEntrySponsorable implementation
//
////////////////////////////////////////////////////////////////////////////////
OwnedEntrySponsorable::OwnedEntrySponsorable(LedgerKey const& key) : mKey(key)
{
    switch (mKey.type())
    {
    case ACCOUNT:
    case TRUSTLINE:
    case OFFER:
    case DATA:
        return;
    case CLAIMABLE_BALANCE:
        throw std::runtime_error("claimable balance is not owned");
    default:
        throw std::runtime_error("unknown entry type");
    }
}

AccountID const&
OwnedEntrySponsorable::getOwnerID() const
{
    switch (mKey.type())
    {
    case ACCOUNT:
        return mKey.account().accountID;
    case TRUSTLINE:
        return mKey.trustLine().accountID;
    case OFFER:
        return mKey.offer().sellerID;
    case DATA:
        return mKey.data().accountID;
    case CLAIMABLE_BALANCE:
        throw std::runtime_error("claimable balance is not owned");
    default:
        throw std::runtime_error("unknown key type");
    }
}

LedgerTxnEntry
OwnedEntrySponsorable::load(AbstractLedgerTxn& ltx)
{
    return ltx.load(mKey);
}

LedgerTxnEntry
OwnedEntrySponsorable::loadOwner(AbstractLedgerTxn& ltx)
{
    return loadAccount(ltx, getOwnerID());
}

LedgerTxnEntry
OwnedEntrySponsorable::loadSponsoring(AbstractLedgerTxn& ltx)
{
    AccountID sponsoringID;
    {
        auto ltxe = load(ltx);
        sponsoringID = *ltxe.current().ext.v1().sponsoringID;
    }
    return loadAccount(ltx, sponsoringID);
}

uint32_t
OwnedEntrySponsorable::getMult(AbstractLedgerTxn& ltx)
{
    switch (mKey.type())
    {
    case ACCOUNT:
        return 2;
    case TRUSTLINE:
    case OFFER:
    case DATA:
        return 1;
    case CLAIMABLE_BALANCE:
        throw std::runtime_error("claimable balance is not owned");
    default:
        throw std::runtime_error("unknown key type");
    }
}

SponsorshipResult
OwnedEntrySponsorable::establishSponsorship(AbstractLedgerTxn& ltxOuter,
                                            AccountID sponsoringID)
{
    return establishSponsorshipHelper(ltxOuter, sponsoringID);
}

SponsorshipResult
OwnedEntrySponsorable::removeSponsorship(AbstractLedgerTxn& ltxOuter)
{
    return removeSponsorshipHelper(ltxOuter, true);
}

SponsorshipResult
OwnedEntrySponsorable::transferSponsorship(AbstractLedgerTxn& ltxOuter,
                                           AccountID newSponsoringID)
{
    return transferSponsorshipHelper(ltxOuter, newSponsoringID);
}

SponsorshipResult
OwnedEntrySponsorable::createWithoutSponsorship(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    if (mKey.type() != ACCOUNT)
    {
        uint32_t const mult = getMult(ltx);

        auto ltxe = loadOwner(ltx);
        auto acc = [&ltxe]() -> AccountEntry& {
            return ltxe.current().data.account();
        };

        auto ltxh = ltx.loadHeader();
        if (ltxh.current().ledgerVersion >=
                FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS &&
            acc().numSubEntries > ACCOUNT_SUBENTRY_LIMIT - mult)
        {
            return SponsorshipResult::TOO_MANY_SUBENTRIES;
        }
        acc().numSubEntries += mult;

        if (getAvailableBalance(ltxh, ltxe) < 0)
        {
            return SponsorshipResult::LOW_RESERVE;
        }
    }
    else
    {
        auto ltxe = loadOwner(ltx); // Equivalent to load(ltx)
        auto acc = [&ltxe]() -> AccountEntry const& {
            return ltxe.current().data.account();
        };

        if (acc().balance < getMinBalance(ltx.loadHeader().current(), acc()))
        {
            return SponsorshipResult::LOW_RESERVE;
        }
    }

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

SponsorshipResult
OwnedEntrySponsorable::createWithSponsorship(AbstractLedgerTxn& ltxOuter,
                                             AccountID const& sponsoringID)
{
    LedgerTxn ltx(ltxOuter);

    auto res = establishSponsorship(ltx, sponsoringID);
    if (res != SponsorshipResult::SUCCESS)
    {
        return res;
    }

    if (mKey.type() != ACCOUNT)
    {
        uint32_t const mult = getMult(ltx);
        if (ltx.loadHeader().current().ledgerVersion >=
                FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS &&
            loadOwner(ltx).current().data.account().numSubEntries >
                ACCOUNT_SUBENTRY_LIMIT - mult)
        {
            return SponsorshipResult::TOO_MANY_SUBENTRIES;
        }
        loadOwner(ltx).current().data.account().numSubEntries += mult;
    }

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

SponsorshipResult
OwnedEntrySponsorable::create(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    std::optional<AccountID> sponsoringID;
    {
        auto sponsorship = loadSponsorship(ltx, getOwnerID());
        if (sponsorship)
        {
            sponsoringID = std::make_optional(sponsorship.currentGeneralized()
                                                  .sponsorshipEntry()
                                                  .sponsoringID);
        }
    }

    auto res = sponsoringID ? createWithSponsorship(ltx, *sponsoringID)
                            : createWithoutSponsorship(ltx);
    if (res != SponsorshipResult::SUCCESS)
    {
        return res;
    }

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

void
OwnedEntrySponsorable::erase(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    if (mKey.type() != ACCOUNT)
    {
        uint32_t mult = getMult(ltx);
        auto ltxeOwner = loadOwner(ltx);
        if (ltxeOwner.current().data.account().numSubEntries < mult)
        {
            throw std::runtime_error("invalid account state");
        }
        ltxeOwner.current().data.account().numSubEntries -= mult;
    }

    if (ltx.loadHeader().current().ledgerVersion >= 14)
    {
        auto ltxe = load(ltx);
        if (ltxe.current().ext.v() == 1 && ltxe.current().ext.v1().sponsoringID)
        {
            if (removeSponsorshipHelper(ltx, false) !=
                SponsorshipResult::SUCCESS)
            {
                throw std::runtime_error(
                    "cannot remove sponsorship while removing entry");
            }
        }
    }

    load(ltx).erase();
    ltx.commit();
}

////////////////////////////////////////////////////////////////////////////////
//
// UnownedEntrySponsorable implementation
//
////////////////////////////////////////////////////////////////////////////////
UnownedEntrySponsorable::UnownedEntrySponsorable(
    ClaimableBalanceID const& balanceID)
    : mBalanceID(balanceID)
{
}

LedgerTxnEntry
UnownedEntrySponsorable::load(AbstractLedgerTxn& ltx)
{
    return loadClaimableBalance(ltx, mBalanceID);
}

LedgerTxnEntry
UnownedEntrySponsorable::loadOwner(AbstractLedgerTxn& ltx)
{
    return {};
}

LedgerTxnEntry
UnownedEntrySponsorable::loadSponsoring(AbstractLedgerTxn& ltx)
{
    AccountID sponsoringID;
    {
        auto ltxe = load(ltx);
        sponsoringID = *ltxe.current().ext.v1().sponsoringID;
    }
    return loadAccount(ltx, sponsoringID);
}

uint32_t
UnownedEntrySponsorable::getMult(AbstractLedgerTxn& ltx)
{
    return load(ltx).current().data.claimableBalance().claimants.size();
}

SponsorshipResult
UnownedEntrySponsorable::establishSponsorship(AbstractLedgerTxn& ltxOuter,
                                              AccountID sponsoringID)
{
    return establishSponsorshipHelper(ltxOuter, sponsoringID);
}

SponsorshipResult
UnownedEntrySponsorable::transferSponsorship(AbstractLedgerTxn& ltxOuter,
                                             AccountID newSponsoringID)
{
    return transferSponsorshipHelper(ltxOuter, newSponsoringID);
}

SponsorshipResult
UnownedEntrySponsorable::create(AbstractLedgerTxn& ltxOuter,
                                AccountID creatorID)
{
    LedgerTxn ltx(ltxOuter);

    AccountID sponsoringID;
    {
        auto sponsorship = loadSponsorship(ltx, creatorID);
        sponsoringID = sponsorship ? sponsorship.currentGeneralized()
                                         .sponsorshipEntry()
                                         .sponsoringID
                                   : creatorID;
    }
    auto res = establishSponsorship(ltx, sponsoringID);
    if (res != SponsorshipResult::SUCCESS)
    {
        return res;
    }

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

void
UnownedEntrySponsorable::erase(AbstractLedgerTxn& ltxOuter)
{
    // Value of checkReserve doesn't matter here because it is only used if
    // loadOwner(), but use false because that is the value that should be used
    // on erase.
    if (removeSponsorshipHelper(ltxOuter, false) != SponsorshipResult::SUCCESS)
    {
        throw std::runtime_error(
            "cannot remove sponsorship while removing entry");
    }
    load(ltxOuter).erase();
}

////////////////////////////////////////////////////////////////////////////////
//
// SignerSponsorable implementation
//
////////////////////////////////////////////////////////////////////////////////
SignerSponsorable::SignerSponsorable(AccountID const& accountID, size_t index)
    : mAccountID(accountID), mIndex(index)
{
}

LedgerTxnEntry
SignerSponsorable::loadOwner(AbstractLedgerTxn& ltx)
{
    return loadAccount(ltx, mAccountID);
}

LedgerTxnEntry
SignerSponsorable::loadSponsoring(AbstractLedgerTxn& ltx)
{
    AccountID sponsoringID;
    {
        auto ltxe = loadOwner(ltx);
        sponsoringID = *ltxe.current()
                            .data.account()
                            .ext.v1()
                            .ext.v2()
                            .signerSponsoringIDs.at(mIndex);
    }
    return loadAccount(ltx, sponsoringID);
}

bool
SignerSponsorable::isSignerSponsored(LedgerTxnEntry const& ltxe)
{
    auto const& ae = ltxe.current().data.account();
    return hasAccountEntryExtV2(ae) &&
           ae.ext.v1().ext.v2().signerSponsoringIDs.at(mIndex);
}

SponsorshipResult
SignerSponsorable::establishSponsorship(AbstractLedgerTxn& ltxOuter,
                                        AccountID sponsoringID)
{
    LedgerTxn ltx(ltxOuter);

    auto ltxh = ltx.loadHeader();
    throwBeforeSponsorshipsEnabled(ltxh.current());

    if (isSignerSponsored(loadOwner(ltx)))
    {
        throw std::runtime_error("bad signer sponsorship");
    }
    uint32_t const mult = 1;

    SponsorshipResult res = SponsorshipResult::SUCCESS;
    if (!addNumSponsoring(ltxh.current(),
                          loadAccount(ltx, sponsoringID).current(), mult, res))
    {
        return res;
    }
    if (!addNumSponsored(ltxh.current(), loadOwner(ltx).current(), mult, res))
    {
        return res;
    }
    prepareAccountEntryExtensionV2(loadOwner(ltx).current().data.account())
        .signerSponsoringIDs.at(mIndex)
        .activate() = sponsoringID;

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

SponsorshipResult
SignerSponsorable::removeSponsorshipHelper(AbstractLedgerTxn& ltxOuter,
                                           bool checkReserve)
{
    LedgerTxn ltx(ltxOuter);

    auto ltxh = ltx.loadHeader();
    throwBeforeSponsorshipsEnabled(ltxh.current());

    if (!isSignerSponsored(loadOwner(ltx)))
    {
        throw std::runtime_error("bad signer sponsorship");
    }
    uint32_t const mult = 1;

    removeNumSponsoring(loadSponsoring(ltx).current(), mult);
    removeNumSponsored(loadOwner(ltx).current(), mult);

    // It is permitted to be beneath the reserve when erasing entries
    if (checkReserve &&
        getAvailableBalance(ltxh.current(), loadOwner(ltx).current()) < 0)
    {
        return SponsorshipResult::LOW_RESERVE;
    }

    prepareAccountEntryExtensionV2(loadOwner(ltx).current().data.account())
        .signerSponsoringIDs.at(mIndex)
        .reset();

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

SponsorshipResult
SignerSponsorable::removeSponsorship(AbstractLedgerTxn& ltxOuter)
{
    return removeSponsorshipHelper(ltxOuter, true);
}

SponsorshipResult
SignerSponsorable::transferSponsorship(AbstractLedgerTxn& ltxOuter,
                                       AccountID newSponsoringID)
{
    LedgerTxn ltx(ltxOuter);

    auto ltxh = ltx.loadHeader();
    throwBeforeSponsorshipsEnabled(ltxh.current());

    if (!isSignerSponsored(loadOwner(ltx)))
    {
        throw std::runtime_error("bad signer sponsorship");
    }
    uint32_t const mult = 1;

    SponsorshipResult res = SponsorshipResult::SUCCESS;
    if (!addNumSponsoring(ltxh.current(),
                          loadAccount(ltx, newSponsoringID).current(), mult,
                          res))
    {
        return res;
    }
    removeNumSponsoring(loadSponsoring(ltx).current(), mult);
    prepareAccountEntryExtensionV2(loadOwner(ltx).current().data.account())
        .signerSponsoringIDs.at(mIndex)
        .activate() = newSponsoringID;

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

SponsorshipResult
SignerSponsorable::create(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    if (ltx.loadHeader().current().ledgerVersion >=
            FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS &&
        loadOwner(ltx).current().data.account().numSubEntries >=
            ACCOUNT_SUBENTRY_LIMIT)
    {
        return SponsorshipResult::TOO_MANY_SUBENTRIES;
    }

    std::optional<AccountID> sponsoringID;
    {
        auto sponsorship = loadSponsorship(ltx, mAccountID);
        if (sponsorship)
        {
            sponsoringID = std::make_optional(sponsorship.currentGeneralized()
                                                  .sponsorshipEntry()
                                                  .sponsoringID);
        }
    }

    if (sponsoringID)
    {
        auto res = establishSponsorship(ltx, *sponsoringID);
        if (res != SponsorshipResult::SUCCESS)
        {
            return res;
        }
    }
    else
    {
        auto ltxh = ltx.loadHeader();
        if (getAvailableBalance(ltxh.current(), loadOwner(ltx).current()) <
            ltxh.current().baseReserve)
        {
            return SponsorshipResult::LOW_RESERVE;
        }
    }
    ++loadOwner(ltx).current().data.account().numSubEntries;

    ltx.commit();
    return SponsorshipResult::SUCCESS;
}

void
SignerSponsorable::erase(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    {
        uint32_t const mult = 1;
        auto ltxeOwner = loadOwner(ltx);
        if (ltxeOwner.current().data.account().numSubEntries < mult)
        {
            throw std::runtime_error("invalid account state");
        }
        ltxeOwner.current().data.account().numSubEntries -= mult;
    }

    if (ltx.loadHeader().current().ledgerVersion >= 14)
    {
        if (isSignerSponsored(loadOwner(ltx)))
        {
            if (removeSponsorshipHelper(ltx, false) !=
                SponsorshipResult::SUCCESS)
            {
                throw std::runtime_error(
                    "cannot remove sponsorship while removing key");
            }
        }

        auto ltxeOwner = loadOwner(ltx);
        if (hasAccountEntryExtV2(ltxeOwner.current().data.account()))
        {
            auto& signerSponsoringIDs =
                getAccountEntryExtensionV2(ltxeOwner.current().data.account())
                    .signerSponsoringIDs;
            signerSponsoringIDs.erase(signerSponsoringIDs.begin() + mIndex);
        }
    }

    auto ltxeOwner = loadOwner(ltx);
    auto& signers = ltxeOwner.current().data.account().signers;
    signers.erase(signers.begin() + mIndex);

    ltx.commit();
}
}
}
