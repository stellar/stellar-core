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

namespace stellar
{
    class OfferFrame : public EntryFrame
    {
        void calculateIndex();
        static void loadOffers(soci::details::prepare_temp_type &prep, std::function<void(const OfferFrame&)> offerProcessor);
    public:

        enum OfferFlags
        {
            PASSIVE_FLAG = 1
        };

        OfferFrame();
        OfferFrame(const LedgerEntry& from);
        void from(const Transaction& tx);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new OfferFrame(*this)); }

        void storeDelete(LedgerDelta &delta, Database& db);
        void storeChange(LedgerDelta &delta, Database& db);
        void storeAdd(LedgerDelta &delta, Database& db);

        int64_t getPrice() const;
        int64_t getAmount() const;
        uint256 const& getAccountID() const;
        Currency& getTakerPays();
        Currency& getTakerGets();
        uint32 getSequence();


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


