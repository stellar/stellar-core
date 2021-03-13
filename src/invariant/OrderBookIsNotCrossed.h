#ifdef BUILD_TESTS
#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"

#include <set>
#include <unordered_map>

namespace stellar
{
// compare two OfferEntry's by price
class OrderBookIsNotCrossed : public Invariant
{
  public:
    struct OfferEntryCmp
    {
        bool operator()(OfferEntry const& a, OfferEntry const& b) const;
    };

    using Orders = std::set<OfferEntry, OfferEntryCmp>;
    using OrderBook = std::unordered_map<AssetPair, Orders, AssetPairHash>;
    using AssetPairSet = std::set<std::pair<Asset, Asset>>;

    explicit OrderBookIsNotCrossed()
        : Invariant(true), mRestoreBeforeNextUpdate(false)
    {
    }

    // The order-book-not-crossed invariant relies on maintaining state across
    // checkOnOperationApply() calls, and there is no general mechanism in the
    // InvariantManager to notify an invariant if an operation was later rolled
    // back.  Therefore, we can reliably check the order-book-not-crossed
    // invariant only when either the calling code uses the
    // snapshotForFuzzer()/resetForFuzzer() mechanism, which currently only the
    // fuzzer does, or in a test specifically of the OrderBookIsNotCrossed
    // invariant, which is aware of its maintaining state.  Therefore, we
    // register and enable the invariant only explicitly, from those places:
    // fuzzing and the invariant's own tests.
    static std::shared_ptr<Invariant>
    registerAndEnableInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta) override;

    OrderBook const&
    getOrderBook() const
    {
        return mOrderBook;
    }

    void snapshotForFuzzer() override;
    void resetForFuzzer() override;

  private:
    // as of right now, since this is only used in fuzzing, the mOrderBook will
    // be empty to start. If used in production, since invariants are
    // configurable, it is likely that this invariant will be enabled with
    // pre-existing ledger and thus the mOrderBook will need a way to sync
    OrderBook mOrderBook;
    OrderBook mOrderBookSnapshot;
    bool mRestoreBeforeNextUpdate;
    void updateOrderBook(LedgerTxnDelta const& ltxd);
    std::string check(AssetPairSet const& assetPairs);
};
}
#endif // BUILD_TESTS
