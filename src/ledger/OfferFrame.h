#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"
#include <functional>

namespace soci
{
namespace details
{
    class prepare_temp_type;
}
}

#define OFFER_PRICE_DIVISOR 10000000

namespace stellar
{
    class OfferFrame : public EntryFrame
    {
        static void loadOffers(soci::details::prepare_temp_type &prep, std::function<void(const OfferFrame&)> offerProcessor);

        int64_t computePrice() const;

        OfferEntry& mOffer;
    public:

        enum OfferFlags
        {
            PASSIVE_FLAG = 1
        };

        OfferFrame();
        OfferFrame(LedgerEntry const& from);
        OfferFrame(OfferFrame const& from);

        OfferFrame& operator=(OfferFrame const& other);
        void from(const Transaction& tx);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new OfferFrame(*this)); }

        Price getPrice() const;
        int64_t getAmount() const;
        uint256 const& getAccountID() const;
        Currency& getTakerPays();
        Currency& getTakerGets();
        uint32 getSequence();

        OfferEntry& getOffer() { return mOffer; }

        // Instance-based overrides of EntryFrame.
        void storeDelete(LedgerDelta &delta, Database& db) override;
        void storeChange(LedgerDelta &delta, Database& db) override;
        void storeAdd(LedgerDelta &delta, Database& db) override;

        // Static helper that doesn't assume an instance.
        static void storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key);

        // database utilities
        static bool loadOffer(const uint256& accountID, uint32_t seq, OfferFrame& retEntry,
            Database& db);

        static void loadBestOffers(size_t numOffers, size_t offset, const Currency& pays,
            const Currency& gets, std::vector<OfferFrame>& retOffers, Database& db);

        static void loadOffers(const uint256& accountID,
            std::vector<OfferFrame>& retOffers, Database& db);

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
    };
}


