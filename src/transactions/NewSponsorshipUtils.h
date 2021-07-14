#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SponsorshipUtils.h"

namespace stellar
{
namespace SponsorshipUtils
{
class Sponsorable
{
  public:
    virtual ~Sponsorable()
    {
    }

    // Pass sponsoringID by value because it is probably coming from an
    // existing LedgerTxnEntry, which will be deactivated
    virtual SponsorshipResult establishSponsorship(AbstractLedgerTxn& ltxOuter,
                                                   AccountID sponsoringID) = 0;

    // Pass newSponsoringID by value because it is probably coming from an
    // existing LedgerTxnEntry, which will be deactivated
    virtual SponsorshipResult
    transferSponsorship(AbstractLedgerTxn& ltxOuter,
                        AccountID newSponsoringID) = 0;
};

class RemovableSponsorable : public Sponsorable
{
  public:
    virtual ~RemovableSponsorable()
    {
    }

    virtual SponsorshipResult
    removeSponsorship(AbstractLedgerTxn& ltxOuter) = 0;
};

template <typename Base> class EntrySponsorable : public Base
{
  protected:
    virtual LedgerTxnEntry load(AbstractLedgerTxn& ltx) = 0;
    virtual LedgerTxnEntry loadOwner(AbstractLedgerTxn& ltx) = 0;
    virtual LedgerTxnEntry loadSponsoring(AbstractLedgerTxn& ltx) = 0;

    virtual uint32_t getMult(AbstractLedgerTxn& ltx) = 0;

  public:
    virtual ~EntrySponsorable()
    {
    }

    SponsorshipResult establishSponsorshipHelper(AbstractLedgerTxn& ltxOuter,
                                                 AccountID const& sponsoringID);

    SponsorshipResult removeSponsorshipHelper(AbstractLedgerTxn& ltxOuter,
                                              bool checkReserve);

    SponsorshipResult
    transferSponsorshipHelper(AbstractLedgerTxn& ltxOuter,
                              AccountID const& newSponsoringID);
};

class OwnedEntrySponsorable : public EntrySponsorable<RemovableSponsorable>
{
    LedgerKey const mKey;

    AccountID const& getOwnerID() const;

    SponsorshipResult createWithoutSponsorship(AbstractLedgerTxn& ltxOuter);

    SponsorshipResult createWithSponsorship(AbstractLedgerTxn& ltxOuter,
                                            AccountID const& sponsoringID);

  protected:
    virtual LedgerTxnEntry load(AbstractLedgerTxn& ltx) override;
    virtual LedgerTxnEntry loadOwner(AbstractLedgerTxn& ltx) override;
    virtual LedgerTxnEntry loadSponsoring(AbstractLedgerTxn& ltx) override;

    virtual uint32_t getMult(AbstractLedgerTxn& ltx) override;

  public:
    explicit OwnedEntrySponsorable(LedgerKey const& key);

    virtual ~OwnedEntrySponsorable()
    {
    }

    virtual SponsorshipResult
    establishSponsorship(AbstractLedgerTxn& ltxOuter,
                         AccountID sponsoringID) override;

    virtual SponsorshipResult
    removeSponsorship(AbstractLedgerTxn& ltxOuter) override;

    virtual SponsorshipResult
    transferSponsorship(AbstractLedgerTxn& ltxOuter,
                        AccountID newSponsoringID) override;

    SponsorshipResult create(AbstractLedgerTxn& ltxOuter);

    void erase(AbstractLedgerTxn& ltxOuter);
};

class UnownedEntrySponsorable : public EntrySponsorable<Sponsorable>
{
    ClaimableBalanceID const mBalanceID;

  protected:
    virtual LedgerTxnEntry load(AbstractLedgerTxn& ltx) override;
    virtual LedgerTxnEntry loadOwner(AbstractLedgerTxn& ltx) override;
    virtual LedgerTxnEntry loadSponsoring(AbstractLedgerTxn& ltx) override;

    virtual uint32_t getMult(AbstractLedgerTxn& ltx) override;

  public:
    explicit UnownedEntrySponsorable(ClaimableBalanceID const& balanceID);

    virtual ~UnownedEntrySponsorable()
    {
    }

    virtual SponsorshipResult
    establishSponsorship(AbstractLedgerTxn& ltxOuter,
                         AccountID sponsoringID) override;

    virtual SponsorshipResult
    transferSponsorship(AbstractLedgerTxn& ltxOuter,
                        AccountID newSponsoringID) override;

    // Pass creatorID by value because it is probably coming from an existing
    // LedgerTxnEntry, which will be deactivated
    SponsorshipResult create(AbstractLedgerTxn& ltxOuter, AccountID creatorID);

    void erase(AbstractLedgerTxn& ltxOuter);
};

std::unique_ptr<Sponsorable> makeSponsorable(LedgerKey const& key);

}
}
