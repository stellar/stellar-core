// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "main/Application.h"
#include "ledger/LedgerMaster.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"
#include "TxTests.h"
#include "util/Timer.h"
#include "database/Database.h"

using namespace stellar;
using namespace stellar::txtest;


typedef std::unique_ptr<Application> appPtr;

// Offer that doesn't cross
// Offer that crosses exactly
// Offer that takes multiple other offers and is cleared
// Offer that takes multiple other offers and remains
// Offer selling STR
// Offer buying STR
// Offer with transfer rate
// Offer for more than you have
// Offer for something you can't hold

TEST_CASE("create offer", "[tx]")
{
    Config const& cfg = getTestConfig();
    Config cfg2(cfg);
    //cfg2.DATABASE = "sqlite3://test.db";
    //cfg2.DATABASE = "postgresql://dbmaster:-island-@localhost/hayashi";
 

    VirtualClock clock;
    Application app(clock, cfg2);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");
    SecretKey gateway = getAccount("gate");

    uint64_t txfee = app.getLedgerMaster().getTxFee();

    const uint64_t paymentAmount = (uint64_t)app.getLedgerMaster().getMinBalance(0);

    Currency idrCur=makeCurrency(gateway,"IDR");
    Currency usdCur = makeCurrency(gateway, "USD");

    applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, 1,txNOACCOUNT);

    // create an accounts
    applyPaymentTx(app, root, a1, 1, paymentAmount);
    applyPaymentTx(app, root, b1, 2, paymentAmount);
    applyPaymentTx(app, root, gateway, 3, paymentAmount);

    applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, 1, txNOTRUST);

    applyTrust(app, a1, gateway, 2, "USD");
    applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, 3, txNOTRUST);

    applyTrust(app, a1, gateway, 4, "IDR");
    applyTrust(app, b1, gateway, 1, "IDR");

    applyOffer(app, b1, idrCur, usdCur, OFFER_PRICE_DIVISOR+1000, 100, 2, txNOTRUST);

    
    // lone offer
    applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, 5);
    OfferFrame offer;
    REQUIRE(app.getDatabase().loadOffer(a1.getPublicKey(), 5, offer));
    REQUIRE(offer.getPrice() == OFFER_PRICE_DIVISOR);
    REQUIRE(offer.getAmount() == 100);
    REQUIRE(offer.getTakerGets().isoCI().currencyCode == idrCur.isoCI().currencyCode);
    REQUIRE(offer.getTakerPays().isoCI().currencyCode == usdCur.isoCI().currencyCode);

    // offer that doesn't cross
    applyOffer(app, b1, usdCur, idrCur, OFFER_PRICE_DIVISOR-1000, 40, 3);
    REQUIRE(app.getDatabase().loadOffer(a1.getPublicKey(), 5, offer));
    REQUIRE(offer.getPrice() == OFFER_PRICE_DIVISOR);
    REQUIRE(offer.getAmount() == 100);
    REQUIRE(offer.getTakerGets().isoCI().currencyCode == idrCur.isoCI().currencyCode);
    REQUIRE(offer.getTakerPays().isoCI().currencyCode == usdCur.isoCI().currencyCode);

    REQUIRE(app.getDatabase().loadOffer(b1.getPublicKey(), 3, offer));
    REQUIRE(offer.getPrice() == OFFER_PRICE_DIVISOR-1000);
    REQUIRE(offer.getAmount() == 40);
    REQUIRE(offer.getTakerPays().isoCI().currencyCode == idrCur.isoCI().currencyCode);
    REQUIRE(offer.getTakerGets().isoCI().currencyCode == usdCur.isoCI().currencyCode);
   
    // Crossing your own offer

    // Too small offers
    // Trying to extract value from an offer
    // Unfunded offer getting cleaned up
    

    // Offer that crosses with some left in the new offer
    // Offer that crosses with none left in the new offer
    // Offer that crosses and takes out both

    // Offer that crosses exactly

    // Offer that takes multiple other offers and is cleared
    // Offer that takes multiple other offers and remains
    // Offer selling STR
    // Offer buying STR
    // Offer with transfer rate
    // Offer for more than you have
    // Offer for something you can't hold
    // Passive offer
    
}
