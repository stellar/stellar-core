#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TxTests.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

#include <map>

namespace stellar
{

class Application;
class TestAccount;
struct OfferEntry;

struct OfferKey
{
    AccountID sellerID;
    uint64_t offerID;

    friend bool operator<(OfferKey const& x, OfferKey const& y);
};

enum class OfferType
{
    ACTIVE,
    PASSIVE
};

struct OfferState
{
    static const OfferState SAME;
    static const OfferState DELETED;

    Asset selling;
    Asset buying;
    Price price;
    int64_t amount{-1};
    OfferType type;

    OfferState()
        : selling{txtest::makeNativeAsset()}
        , buying{txtest::makeNativeAsset()}
        , price{Price{1, 1}}
        , amount{-1}
    {
    }
    OfferState(Asset selling, Asset buying, Price price, int64_t amount,
               OfferType type = OfferType::ACTIVE)
        : selling{std::move(selling)}
        , buying{std::move(buying)}
        , price{std::move(price)}
        , amount{amount}
        , type{type}
    {
    }

    OfferState(OfferState const& os) = default;
    OfferState& operator=(OfferState const& os) = default;

    OfferState(OfferEntry const& entry);
    friend bool operator==(OfferState const& x, OfferState const& y);
};

struct TestMarketOffer
{
    OfferKey key;
    OfferState state;

    ClaimOfferAtom exchanged(int64_t sold, int64_t bought) const;
};

struct TestMarketBalance
{
    Asset asset;
    int64_t balance;
};

struct TestMarketBalances
{
    SecretKey key;
    std::vector<TestMarketBalance> balances;
};

class TestMarket
{
  public:
    explicit TestMarket(Application& app);

    TestMarketOffer
    addOffer(TestAccount& account, OfferState const& state,
             OfferState const& finishedState = OfferState::SAME);

    TestMarketOffer
    updateOffer(TestAccount& account, uint64_t id, OfferState const& state,
                OfferState const& finishedState = OfferState::SAME);

    void requireChanges(std::vector<TestMarketOffer> const& changes,
                        std::function<void()> const& f);
    TestMarketOffer
    requireChangesWithOffer(std::vector<TestMarketOffer> changes,
                            std::function<TestMarketOffer()> const& f);

    void requireBalances(std::vector<TestMarketBalances> const& balances);

    void checkCurrentOffers();

  private:
    Application& mApp;
    std::map<OfferKey, OfferState> mOffers;
    uint64_t mLastAddedID{0};

    void checkState(std::map<OfferKey, OfferState> const& offers,
                    std::vector<OfferKey> const& deletedOffers);
};
}
