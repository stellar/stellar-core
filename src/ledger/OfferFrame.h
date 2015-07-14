#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <functional>

namespace soci
{
class session;
namespace details
{
class prepare_temp_type;
}
}

#define OFFER_PRICE_DIVISOR 10000000

namespace stellar
{
class ManageOfferOpFrame;

class OfferFrame : public EntryFrame
{
    static void
    loadOffers(soci::details::prepare_temp_type& prep,
               std::function<void(LedgerEntry const&)> offerProcessor);

    int64_t computePrice() const;

    OfferEntry& mOffer;

    OfferFrame(OfferFrame const& from);

  public:
    typedef std::shared_ptr<OfferFrame> pointer;

    enum OfferFlags
    {
        PASSIVE_FLAG = 1
    };

    OfferFrame();
    OfferFrame(LedgerEntry const& from);

    OfferFrame& operator=(OfferFrame const& other);
    static OfferFrame::pointer from(AccountID const & account, ManageOfferOp const& op);

    EntryFrame::pointer
    copy() const override
    {
        return EntryFrame::pointer(new OfferFrame(*this));
    }

    Price const& getPrice() const;
    int64_t getAmount() const;
    AccountID const& getAccountID() const;
    Currency const& getTakerPays() const;
    Currency const& getTakerGets() const;
    uint64 getOfferID() const;
    uint32 getFlags() const;

    OfferEntry const&
    getOffer() const
    {
        return mOffer;
    }
    OfferEntry&
    getOffer()
    {
        return mOffer;
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) const override;
    void storeAdd(LedgerDelta& delta, Database& db) const override;

    // Static helpers that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);
    static uint64_t countObjects(soci::session& sess);

    // database utilities
    static pointer loadOffer(AccountID const& accountID, uint64_t offerID,
                             Database& db);

    static void loadBestOffers(size_t numOffers, size_t offset,
                               Currency const& pays, Currency const& gets,
                               std::vector<OfferFrame::pointer>& retOffers,
                               Database& db);

    static void loadOffers(AccountID const& accountID,
                           std::vector<OfferFrame::pointer>& retOffers,
                           Database& db);

    static void dropAll(Database& db);
    static const char* kSQLCreateStatement1;
    static const char* kSQLCreateStatement2;
    static const char* kSQLCreateStatement3;
    static const char* kSQLCreateStatement4;
};
}
