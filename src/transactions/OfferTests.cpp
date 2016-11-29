// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "util/Timer.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Offer that takes multiple other offers and remains
// Offer selling XLM
// Offer buying XLM
// Offer with transfer rate
// Offer for more than you have
// Offer for something you can't hold
// Offer with line full (both accounts)

TEST_CASE("create offer", "[tx][offers]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    Hash const& networkID = app.getNetworkID();
    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);

    const int64_t assetMultiplier = 1000000;

    int64_t trustLineBalance = 100000 * assetMultiplier;
    int64_t trustLineLimit = trustLineBalance * 10;

    int64_t txfee = app.getLedgerManager().getTxFee();

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        app.getLedgerManager().getMinBalance(2) + 20 * txfee;

    // sets up gateway account
    auto gateway = root.create("gateway", minBalance2 * 10);

    Asset idrCur = makeAsset(gateway, "IDR");
    Asset usdCur = makeAsset(gateway, "USD");

    const Price oneone(1, 1);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    SECTION("account a1 does not exist")
    {
        auto txFrame =
            manageOfferOp(networkID, 0, getAccount("a1"), idrCur, usdCur, oneone, 100, 1);

        applyCheck(txFrame, delta, app);
        REQUIRE(txFrame->getResultCode() == txNO_ACCOUNT);
    }

    SECTION("passive offer")
    {
        auto a1 = root.create("A", minBalance2 * 2);
        auto b1 = root.create("B", minBalance2 * 2);

        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "IDR", trustLineLimit);
        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "USD", trustLineLimit);
        applyChangeTrust(app, b1, gateway, b1.nextSequenceNumber(), "IDR", trustLineLimit);
        applyChangeTrust(app, b1, gateway, b1.nextSequenceNumber(), "USD", trustLineLimit);

        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway.nextSequenceNumber(),
                             trustLineBalance);
        applyCreditPaymentTx(app, gateway, b1, usdCur, gateway.nextSequenceNumber(),
                             trustLineBalance);

        uint64_t firstOfferID = delta.getHeaderFrame().getLastGeneratedID() + 1;
        auto txFrame = manageOfferOp(networkID, 0, a1, idrCur, usdCur, oneone,
                                     100 * assetMultiplier, a1.nextSequenceNumber());
        REQUIRE(applyCheck(txFrame, delta, app));

        // offer2 is a passive offer
        uint64_t secondOfferID =
            delta.getHeaderFrame().getLastGeneratedID() + 1;
        txFrame = createPassiveOfferOp(networkID, b1, usdCur, idrCur, oneone,
                                       100 * assetMultiplier, b1.nextSequenceNumber());
        REQUIRE(applyCheck(txFrame, delta, app));

        REQUIRE(secondOfferID == (firstOfferID + 1));

        // offer1 didn't change
        OfferFrame::pointer offer = loadOffer(a1, firstOfferID, app);
        REQUIRE(offer->getAmount() == (100 * assetMultiplier));
        REQUIRE((offer->getFlags() & PASSIVE_FLAG) == 0);

        offer = loadOffer(b1, secondOfferID, app);
        REQUIRE(offer->getAmount() == (100 * assetMultiplier));
        REQUIRE((offer->getFlags() & PASSIVE_FLAG) != 0);

        const Price highPrice(100, 99);
        const Price lowPrice(99, 100);

        SECTION("creates a passive offer with a better price")
        {
            uint64_t thirdOfferID =
                delta.getHeaderFrame().getLastGeneratedID() + 1;
            txFrame =
                createPassiveOfferOp(networkID, b1, usdCur, idrCur, lowPrice,
                                     100 * assetMultiplier, b1.nextSequenceNumber());
            applyCheck(txFrame, delta, app);

            // offer1 is taken, offer3 was not created
            REQUIRE(!loadOffer(a1, firstOfferID, app, false));
            REQUIRE(!loadOffer(b1, thirdOfferID, app, false));
        }
        SECTION("modify existing passive offer")
        {
            SECTION("modify high")
            {
                txFrame =
                    manageOfferOp(networkID, secondOfferID, b1, usdCur, idrCur,
                                  highPrice, 100 * assetMultiplier, b1.nextSequenceNumber());
                applyCheck(txFrame, delta, app);

                offer = loadOffer(a1, firstOfferID, app);
                REQUIRE(offer->getAmount() == (100 * assetMultiplier));
                REQUIRE((offer->getFlags() & PASSIVE_FLAG) == 0);

                offer = loadOffer(b1, secondOfferID, app);
                REQUIRE(offer->getAmount() == (100 * assetMultiplier));
                REQUIRE(offer->getPrice() == highPrice);
                REQUIRE((offer->getFlags() & PASSIVE_FLAG) != 0);
            }
            SECTION("modify low")
            {
                txFrame =
                    manageOfferOp(networkID, secondOfferID, b1, usdCur, idrCur,
                                  lowPrice, 100 * assetMultiplier, b1.nextSequenceNumber());
                applyCheck(txFrame, delta, app);

                REQUIRE(!loadOffer(a1, firstOfferID, app, false));
                REQUIRE(!loadOffer(b1, secondOfferID, app, false));
            }
        }
    }

    SECTION("negative offer creation tests")
    {
        auto a1 = root.create("A", minBalance2);

        // sell IDR for USD

        // missing IDR trust
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_SELL_NO_TRUST);

        // no issuer for selling
        SecretKey gateway2 = getAccount("other gate");
        Asset idrCur2 = makeAsset(gateway2, "IDR");
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur2, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_SELL_NO_ISSUER);

        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "IDR", trustLineLimit);

        // can't sell IDR if account doesn't have any
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_UNDERFUNDED);

        // fund a1 with some IDR
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway.nextSequenceNumber(),
                             trustLineLimit);

        // missing USD trust
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_BUY_NO_TRUST);

        // no issuer for buying
        Asset usdCur2 = makeAsset(gateway2, "USD");
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur2, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_BUY_NO_ISSUER);

        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "USD", trustLineLimit);

        // need sufficient XLM funds to create an offer
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_LOW_RESERVE);

        // add some funds to create the offer
        applyPaymentTx(app, root, a1, root.nextSequenceNumber(), minBalance2);

        // can't receive more of what we're trying to buy
        // first, fill the trust line to the limit
        applyCreditPaymentTx(app, gateway, a1, usdCur, gateway.nextSequenceNumber(),
                             trustLineLimit);
        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_LINE_FULL);

        // try to overflow
        // first moves the limit and balance to INT64_MAX
        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "USD", INT64_MAX);
        applyCreditPaymentTx(app, gateway, a1, usdCur, gateway.nextSequenceNumber(),
                             INT64_MAX - trustLineLimit);

        applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur, oneone,
                                   100, a1.nextSequenceNumber(), MANAGE_OFFER_LINE_FULL);

        // there should be no pending offer at this point in the system
        OfferFrame offer;
        for (int i = 0; i < 9; i++)
        {
            REQUIRE(!loadOffer(a1, i, app, false));
        }
    }

    SECTION("offer manipulation")
    {
        const int64_t minBalanceA = app.getLedgerManager().getMinBalance(3);

        auto a1 = root.create("A", minBalanceA + 10000);

        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "USD", trustLineLimit);
        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "IDR", trustLineLimit);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway.nextSequenceNumber(),
                             trustLineBalance);

        auto res = applyCreateOfferWithResult(app, delta, 0, a1, idrCur, usdCur,
                                              oneone, 100, a1.nextSequenceNumber(),
                                              MANAGE_OFFER_SUCCESS);

        auto offer = res.success().offer.offer();
        auto orgOffer = loadOffer(a1, offer.offerID, app);

        SECTION("Cancel offer")
        {
            auto cancelCheck = [&]()
            {
                auto cancelRes = applyCreateOfferWithResult(
                    app, delta, offer.offerID, a1, idrCur, usdCur, oneone, 0,
                    a1.nextSequenceNumber(), MANAGE_OFFER_SUCCESS);

                REQUIRE(cancelRes.success().offer.effect() ==
                        MANAGE_OFFER_DELETED);
                REQUIRE(!loadOffer(a1, offer.offerID, app, false));
            };
            SECTION("Typical")
            {
                cancelCheck();
            }
            SECTION("selling trust line")
            {
                // not having a balance should not stop deleting the offer
                applyCreditPaymentTx(app, a1, gateway, idrCur, a1.nextSequenceNumber(),
                                     trustLineBalance);
                SECTION("empty")
                {
                    cancelCheck();
                }
                SECTION("Deleted trust line")
                {
                    applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "IDR", 0);
                    cancelCheck();
                }
            }
            SECTION("buying trust line")
            {
                SECTION("trust line full")
                {
                    // having a trust line full should not stop from deleting
                    // the
                    // offer
                    applyCreditPaymentTx(app, gateway, a1, usdCur,
                                         gateway.nextSequenceNumber(), trustLineLimit);
                    cancelCheck();
                }
                SECTION("Deleted trust line")
                {
                    applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "USD", 0);
                    cancelCheck();
                }
            }
        }
        SECTION("negative tests (manipulation)")
        {
            SECTION("Delete non existant offer")
            {
                auto bogusOfferID = offer.offerID + 1;
                auto r = applyCreateOfferWithResult(
                    app, delta, bogusOfferID, a1, idrCur, usdCur, oneone, 0,
                    a1.nextSequenceNumber(), MANAGE_OFFER_NOT_FOUND);
            }
        }
        SECTION("Update price")
        {
            const Price onetwo(1, 2);
            auto updateRes = applyCreateOfferWithResult(
                app, delta, offer.offerID, a1, idrCur, usdCur, onetwo, 100,
                a1.nextSequenceNumber(), MANAGE_OFFER_SUCCESS);

            REQUIRE(updateRes.success().offer.effect() == MANAGE_OFFER_UPDATED);
            auto modOffer = loadOffer(a1, offer.offerID, app);
            REQUIRE(modOffer->getOffer().price == onetwo);
            modOffer->getOffer().price = oneone;
            REQUIRE(orgOffer->getOffer() == modOffer->getOffer());
        }
        SECTION("Update amount")
        {
            auto updateRes = applyCreateOfferWithResult(
                app, delta, offer.offerID, a1, idrCur, usdCur, oneone, 10,
                a1.nextSequenceNumber(), MANAGE_OFFER_SUCCESS);

            REQUIRE(updateRes.success().offer.effect() == MANAGE_OFFER_UPDATED);
            auto modOffer = loadOffer(a1, offer.offerID, app);
            REQUIRE(modOffer->getOffer().amount == 10);
            modOffer->getOffer().amount = 100;
            REQUIRE(orgOffer->getOffer() == modOffer->getOffer());
        }
        SECTION("Update selling/buying assets")
        {
            // needs usdCur
            applyCreditPaymentTx(app, gateway, a1, usdCur, gateway.nextSequenceNumber(),
                                 trustLineBalance);

            // swap selling and buying
            auto updateRes = applyCreateOfferWithResult(
                app, delta, offer.offerID, a1, usdCur, idrCur, oneone, 100,
                a1.nextSequenceNumber(), MANAGE_OFFER_SUCCESS);

            REQUIRE(updateRes.success().offer.effect() == MANAGE_OFFER_UPDATED);
            auto modOffer = loadOffer(a1, offer.offerID, app);
            REQUIRE(modOffer->getOffer().selling == usdCur);
            REQUIRE(modOffer->getOffer().buying == idrCur);
            std::swap(modOffer->getOffer().buying,
                      modOffer->getOffer().selling);
            REQUIRE(orgOffer->getOffer() == modOffer->getOffer());
        }
    }

    // minimum balance to hold
    // 2 trust lines and one offer
    const int64_t minBalance3 = app.getLedgerManager().getMinBalance(3);

    SECTION("a1 setup properly")
    {
        OfferFrame::pointer offer;

        // fund a1 with some IDR and XLM

        const int nbOffers = 22;

        const int64_t minBalanceA =
            app.getLedgerManager().getMinBalance(3 + nbOffers);

        auto a1 = root.create("A", minBalanceA + 10000);

        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "USD", trustLineLimit);
        applyChangeTrust(app, a1, gateway, a1.nextSequenceNumber(), "IDR", trustLineLimit);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway.nextSequenceNumber(),
                             trustLineBalance);
        SECTION("Native offers")
        {
            Asset xlmCur;
            xlmCur.type(AssetType::ASSET_TYPE_NATIVE);

            const Price somePrice(3, 2);
            SECTION("IDR -> XLM")
            {
                applyCreateOffer(app, delta, 0, a1, xlmCur, idrCur, somePrice,
                                 100 * assetMultiplier, a1.nextSequenceNumber());
            }
            SECTION("XLM -> IDR")
            {
                applyCreateOffer(app, delta, 0, a1, idrCur, xlmCur, somePrice,
                                 100 * assetMultiplier, a1.nextSequenceNumber());
            }
        }

        SECTION("multiple offers tests")
        {
            // create nbOffers
            std::vector<uint64_t> a1OfferID;
            const Price usdPriceOfferA(3, 2);

            for (int i = 0; i < nbOffers; i++)
            {

                // offer is sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy USD
                // @
                // 1.5
                uint64_t newOfferID = applyCreateOffer(
                    app, delta, 0, a1, idrCur, usdCur, usdPriceOfferA,
                    100 * assetMultiplier, a1.nextSequenceNumber());

                offer = loadOffer(a1, newOfferID, app);

                a1OfferID.push_back(newOfferID);

                // verifies that the offer was created as expected
                REQUIRE(offer->getPrice() == usdPriceOfferA);
                REQUIRE(offer->getAmount() == 100 * assetMultiplier);
                REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                        idrCur.alphaNum4().assetCode);
                REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                        usdCur.alphaNum4().assetCode);
            }

            auto b1 = root.create("B", minBalance3 + 10000);

            applyChangeTrust(app, b1, gateway, b1.nextSequenceNumber(), "IDR", trustLineLimit);
            applyChangeTrust(app, b1, gateway, b1.nextSequenceNumber(), "USD", trustLineLimit);

            const Price twoone(2, 1);

            SECTION("offer that doesn't cross")
            {
                applyCreditPaymentTx(app, gateway, b1, usdCur, gateway.nextSequenceNumber(),
                                     20000 * assetMultiplier);

                // offer is sell 40 USD for 80 IDR ; sell USD @ 2

                uint64_t offerID =
                    applyCreateOffer(app, delta, 0, b1, usdCur, idrCur, twoone,
                                     40 * assetMultiplier, b1.nextSequenceNumber());

                // verifies that the offer was created properly
                offer = loadOffer(b1, offerID, app);
                REQUIRE(offer->getPrice() == twoone);
                REQUIRE(offer->getAmount() == 40 * assetMultiplier);
                REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                        idrCur.alphaNum4().assetCode);
                REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                        usdCur.alphaNum4().assetCode);

                // and that a1 offers were not touched
                for (auto a1Offer : a1OfferID)
                {
                    offer = loadOffer(a1, a1Offer, app);
                    REQUIRE(offer->getPrice() == usdPriceOfferA);
                    REQUIRE(offer->getAmount() == 100 * assetMultiplier);
                    REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                            usdCur.alphaNum4().assetCode);
                    REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                            idrCur.alphaNum4().assetCode);
                }
            }

            SECTION("Offer crossing own offer")
            {
                applyCreditPaymentTx(app, gateway, a1, usdCur, gateway.nextSequenceNumber(),
                                     20000 * assetMultiplier);

                // ensure we could receive proceeds from the offer
                applyCreditPaymentTx(app, a1, gateway, idrCur, a1.nextSequenceNumber(),
                                     100000 * assetMultiplier);

                // offer is sell 150 USD for 100 IDR; sell USD @ 1.5 / buy IRD @
                // 0.66
                Price exactCross(usdPriceOfferA.d, usdPriceOfferA.n);

                uint64_t beforeID = delta.getHeaderFrame().getLastGeneratedID();
                applyCreateOfferWithResult(app, delta, 0, a1, usdCur, idrCur,
                                           exactCross, 150 * assetMultiplier,
                                           a1.nextSequenceNumber(), MANAGE_OFFER_CROSS_SELF);
                REQUIRE(beforeID ==
                        delta.getHeaderFrame().getLastGeneratedID());

                for (auto a1Offer : a1OfferID)
                {
                    offer = loadOffer(a1, a1Offer, app);
                    REQUIRE(offer->getPrice() == usdPriceOfferA);
                    REQUIRE(offer->getAmount() == 100 * assetMultiplier);
                    REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                            usdCur.alphaNum4().assetCode);
                    REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                            idrCur.alphaNum4().assetCode);
                }
            }

            SECTION("Offer that crosses exactly")
            {
                applyCreditPaymentTx(app, gateway, b1, usdCur, gateway.nextSequenceNumber(),
                                     20000 * assetMultiplier);

                // offer is sell 150 USD for 100 USD; sell USD @ 1.5 / buy IRD @
                // 0.66
                Price exactCross(usdPriceOfferA.d, usdPriceOfferA.n);

                uint64_t expectedID =
                    delta.getHeaderFrame().getLastGeneratedID() + 1;
                auto const& res = applyCreateOfferWithResult(
                    app, delta, 0, b1, usdCur, idrCur, exactCross,
                    150 * assetMultiplier, b1.nextSequenceNumber());

                REQUIRE(res.success().offer.effect() == MANAGE_OFFER_DELETED);

                // verifies that the offer was not created
                REQUIRE(!loadOffer(b1, expectedID, app, false));

                // and the state of a1 offers
                for (int i = 0; i < nbOffers; i++)
                {
                    uint64_t a1Offer = a1OfferID[i];

                    if (i == 0)
                    {
                        // first offer was taken
                        REQUIRE(!loadOffer(a1, a1Offer, app, false));
                    }
                    else
                    {
                        offer = loadOffer(a1, a1Offer, app);
                        REQUIRE(offer->getPrice() == usdPriceOfferA);
                        REQUIRE(offer->getAmount() == 100 * assetMultiplier);
                        REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                                usdCur.alphaNum4().assetCode);
                        REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                                idrCur.alphaNum4().assetCode);
                    }
                }
            }

            TrustFrame::pointer line;
            line = loadTrustLine(a1, usdCur, app);
            int64_t a1_usd = line->getBalance();

            line = loadTrustLine(a1, idrCur, app);
            int64_t a1_idr = line->getBalance();

            const Price onetwo(1, 2);

            SECTION("Offer that takes multiple other offers and is cleared")
            {
                applyCreditPaymentTx(app, gateway, b1, usdCur, gateway.nextSequenceNumber(),
                                     20000 * assetMultiplier);

                line = loadTrustLine(b1, usdCur, app);
                int64_t b1_usd = line->getBalance();

                line = loadTrustLine(b1, idrCur, app);
                int64_t b1_idr = line->getBalance();

                uint64_t expectedID =
                    delta.getHeaderFrame().getLastGeneratedID() + 1;
                // offer is sell 1010 USD for 505 IDR; sell USD @ 0.5
                auto const& res = applyCreateOfferWithResult(
                    app, delta, 0, b1, usdCur, idrCur, onetwo,
                    1010 * assetMultiplier, b1.nextSequenceNumber());

                REQUIRE(res.success().offer.effect() == MANAGE_OFFER_DELETED);
                // verify that the offer was not created
                REQUIRE(!loadOffer(b1, expectedID, app, false));

                // Offers are: sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy
                // USD
                // @ 1.5
                // first 6 offers get taken for 6*150=900 USD, gets 600 IDR in
                // return
                // offer #7 : has 110 USD available
                //    -> can claim partial offer 100*110/150 = 73.333 ; ->
                //    26.66666
                //    left
                // 8 .. untouched
                // the USDs were sold at the (better) rate found in the original
                // offers
                int64_t usdRecv = 1010 * assetMultiplier;

                int64_t idrSend = bigDivide(usdRecv, 2, 3);

                for (int i = 0; i < nbOffers; i++)
                {
                    uint64_t a1Offer = a1OfferID[i];

                    if (i < 6)
                    {
                        // first 6 offers are taken
                        REQUIRE(!loadOffer(a1, a1Offer, app, false));
                    }
                    else
                    {
                        // others are untouched
                        offer = loadOffer(a1, a1Offer, app);
                        REQUIRE(offer->getPrice() == usdPriceOfferA);
                        REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                                usdCur.alphaNum4().assetCode);
                        REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                                idrCur.alphaNum4().assetCode);
                        if (i == 6)
                        {
                            int64_t expected =
                                100 * assetMultiplier -
                                (idrSend - 6 * 100 * assetMultiplier);
                            checkAmounts(expected, offer->getAmount());
                        }
                        else
                        {
                            REQUIRE(offer->getAmount() ==
                                    100 * assetMultiplier);
                        }
                    }
                }

                // check balances
                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(a1_usd + usdRecv, line->getBalance());

                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(a1_idr - idrSend, line->getBalance());

                // buyer may have paid a bit more to cross offers
                line = loadTrustLine(b1, usdCur, app);
                checkAmounts(line->getBalance(), b1_usd - usdRecv);

                line = loadTrustLine(b1, idrCur, app);
                checkAmounts(line->getBalance(), b1_idr + idrSend);
            }

            SECTION("Trying to extract value from an offer")
            {
                applyCreditPaymentTx(app, gateway, b1, usdCur, gateway.nextSequenceNumber(),
                                     20000 * assetMultiplier);

                line = loadTrustLine(b1, usdCur, app);
                int64_t b1_usd = line->getBalance();

                line = loadTrustLine(b1, idrCur, app);
                int64_t b1_idr = line->getBalance();

                // the USDs were sold at the (better) rate found in the original
                // offers
                int64_t usdRecv = 10 * assetMultiplier;

                int64_t idrSend = bigDivide(usdRecv, 2, 3);

                for (int j = 0; j < 10; j++)
                {
                    // offer is sell 1 USD for 0.5 IDR; sell USD @ 0.5

                    uint64_t wouldCreateID =
                        delta.getHeaderFrame().getLastGeneratedID() + 1;
                    auto const& res = applyCreateOfferWithResult(
                        app, delta, 0, b1, usdCur, idrCur, onetwo,
                        1 * assetMultiplier, b1.nextSequenceNumber());

                    REQUIRE(res.success().offer.effect() ==
                            MANAGE_OFFER_DELETED);
                    REQUIRE(!loadOffer(b1, wouldCreateID, app, false));
                }

                for (int i = 0; i < nbOffers; i++)
                {
                    uint64_t a1Offer = a1OfferID[i];

                    offer = loadOffer(a1, a1Offer, app);

                    REQUIRE(offer->getBuying().alphaNum4().assetCode ==
                            usdCur.alphaNum4().assetCode);
                    REQUIRE(offer->getSelling().alphaNum4().assetCode ==
                            idrCur.alphaNum4().assetCode);

                    if (i == 0)
                    {
                        int64_t expected = 100 * assetMultiplier - idrSend;
                        checkAmounts(expected, offer->getAmount(), 10);
                    }
                    else
                    {
                        REQUIRE(offer->getAmount() == 100 * assetMultiplier);
                    }
                }

                // check balances

                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(a1_usd + usdRecv, line->getBalance(), 10);

                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(a1_idr - idrSend, line->getBalance(), 10);

                line = loadTrustLine(b1, usdCur, app);
                checkAmounts(line->getBalance(), b1_usd - usdRecv, 10);

                line = loadTrustLine(b1, idrCur, app);
                checkAmounts(line->getBalance(), b1_idr + idrSend, 10);
            }

            SECTION("Offer that takes multiple other offers and remains")
            {
                applyCreditPaymentTx(app, gateway, b1, usdCur, gateway.nextSequenceNumber(),
                                     20000 * assetMultiplier);

                line = loadTrustLine(b1, usdCur, app);
                int64_t b1_usd = line->getBalance();

                line = loadTrustLine(b1, idrCur, app);
                int64_t b1_idr = line->getBalance();

                auto c1 = root.create("C", minBalance3 + 10000);

                // inject also an offer that should get cleaned up
                uint64_t cOfferID = 0;
                {
                    applyChangeTrust(app, c1, gateway, c1.nextSequenceNumber(), "IDR",
                                     trustLineLimit);
                    applyChangeTrust(app, c1, gateway, c1.nextSequenceNumber(), "USD",
                                     trustLineLimit);
                    applyCreditPaymentTx(app, gateway, c1, idrCur,
                                         gateway.nextSequenceNumber(),
                                         20000 * assetMultiplier);

                    // matches the offer from A
                    cOfferID = applyCreateOffer(
                        app, delta, 0, c1, idrCur, usdCur, usdPriceOfferA,
                        100 * assetMultiplier, c1.nextSequenceNumber());
                    // drain account
                    applyCreditPaymentTx(app, c1, gateway, idrCur, c1.nextSequenceNumber(),
                                         20000 * assetMultiplier);
                    // offer should still be there
                    loadOffer(c1, cOfferID, app);
                }

                // offer is sell 10000 USD for 5000 IDR; sell USD @ 0.5

                int64_t usdBalanceForSale = 10000 * assetMultiplier;
                uint64_t offerID =
                    applyCreateOffer(app, delta, 0, b1, usdCur, idrCur, onetwo,
                                     usdBalanceForSale, b1.nextSequenceNumber());

                offer = loadOffer(b1, offerID, app);

                // Offers are: sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy
                // USD
                // @ 1.5
                int64_t usdRecv = 150 * assetMultiplier * nbOffers;
                int64_t idrSend = bigDivide(usdRecv, 2, 3);

                int64_t expected = usdBalanceForSale - usdRecv;

                checkAmounts(expected, offer->getAmount());

                // check that the bogus offer was cleared
                REQUIRE(!loadOffer(c1, cOfferID, app, false));

                for (int i = 0; i < nbOffers; i++)
                {
                    uint64_t a1Offer = a1OfferID[i];
                    REQUIRE(!loadOffer(a1, a1Offer, app, false));
                }

                // check balances
                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(a1_usd + usdRecv, line->getBalance());

                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(a1_idr - idrSend, line->getBalance());

                // buyer may have paid a bit more to cross offers
                line = loadTrustLine(b1, usdCur, app);
                checkAmounts(line->getBalance(), b1_usd - usdRecv);

                line = loadTrustLine(b1, idrCur, app);
                checkAmounts(line->getBalance(), b1_idr + idrSend);
            }
        }
        SECTION("limits and issuers")
        {
            const Price usdPriceOfferA(3, 2);
            // offer is sell 100 IDR for 150 USD; buy USD @ 1.5 = sell IRD @
            // 0.66
            uint64_t offerA1 = applyCreateOffer(
                app, delta, 0, a1, idrCur, usdCur, usdPriceOfferA,
                100 * assetMultiplier, a1.nextSequenceNumber());

            offer = loadOffer(a1, offerA1, app);

            SECTION("multiple parties")
            {
                // b1 sells the same thing
                auto b1 = root.create("B", minBalance3 + 10000);

                applyChangeTrust(app, b1, gateway, b1.nextSequenceNumber(), "IDR",
                                 trustLineLimit);
                applyChangeTrust(app, b1, gateway, b1.nextSequenceNumber(), "USD",
                                 trustLineLimit);

                applyCreditPaymentTx(app, gateway, b1, idrCur, gateway.nextSequenceNumber(),
                                     trustLineBalance);

                uint64_t offerB1 = applyCreateOffer(
                    app, delta, 0, b1, idrCur, usdCur, usdPriceOfferA,
                    100 * assetMultiplier, b1.nextSequenceNumber());

                offer = loadOffer(b1, offerB1, app);

                auto c1 = root.create("C", minBalanceA + 10000);

                applyChangeTrust(app, c1, gateway, c1.nextSequenceNumber(), "USD",
                                 trustLineLimit);
                applyChangeTrust(app, c1, gateway, c1.nextSequenceNumber(), "IDR",
                                 trustLineLimit);

                applyCreditPaymentTx(app, gateway, c1, usdCur, gateway.nextSequenceNumber(),
                                     trustLineBalance);
                SECTION("Creates an offer but reaches limit while selling")
                {
                    // fund C such that it's 150 IDR below its limit
                    applyCreditPaymentTx(
                        app, gateway, c1, idrCur, gateway.nextSequenceNumber(),
                        trustLineLimit - 150 * assetMultiplier);

                    // try to create an offer:
                    // it will cross with the offers from A and B but will stop
                    // when
                    // C1's limit is reached.
                    // it should still be able to buy 150 IDR / sell 225 USD

                    // offer is buy 200 IDR for 300 USD; buy IDR @ 0.66 USD
                    // -> sell USD @ 1.5 IDR
                    const Price idrPriceOfferC(2, 3);
                    auto offerC1Res = applyCreateOfferWithResult(
                        app, delta, 0, c1, usdCur, idrCur, idrPriceOfferC,
                        300 * assetMultiplier, c1.nextSequenceNumber());
                    // offer consumed offers but was not created
                    REQUIRE(offerC1Res.success().offer.effect() ==
                            MANAGE_OFFER_DELETED);

                    TrustFrame::pointer line;

                    // check balances

                    // A1's offer was taken entirely
                    line = loadTrustLine(a1, usdCur, app);
                    checkAmounts(150 * assetMultiplier, line->getBalance());

                    line = loadTrustLine(a1, idrCur, app);
                    checkAmounts(trustLineBalance - 100 * assetMultiplier,
                                 line->getBalance());

                    // B1's offer was partially taken
                    // buyer may have paid a bit more to cross offers
                    line = loadTrustLine(b1, usdCur, app);
                    checkAmounts(line->getBalance(), 75 * assetMultiplier);

                    line = loadTrustLine(b1, idrCur, app);
                    checkAmounts(line->getBalance(),
                                 trustLineBalance - 50 * assetMultiplier);

                    // C1
                    line = loadTrustLine(c1, usdCur, app);
                    checkAmounts(line->getBalance(),
                                 trustLineBalance - 225 * assetMultiplier);

                    line = loadTrustLine(c1, idrCur, app);
                    checkAmounts(line->getBalance(), trustLineLimit);
                }
                SECTION("Create an offer, top seller has limits")
                {
                    SECTION("Creates an offer, top seller not authorized")
                    {
                        // sets up the secure gateway account for USD
                        auto secgateway = root.create("secure", minBalance2);

                        Asset secUsdCur = makeAsset(secgateway, "USD");
                        Asset secIdrCur = makeAsset(secgateway, "IDR");

                        uint32_t setFlags =
                            AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;

                        applySetOptions(app, secgateway, secgateway.nextSequenceNumber(), nullptr,
                                        &setFlags, nullptr, nullptr, nullptr,
                                        nullptr);

                        // setup d1
                        auto d1 = root.create("D", minBalance3 + 10000);

                        applyChangeTrust(app, d1, secgateway, d1.nextSequenceNumber(), "IDR",
                                         trustLineLimit);
                        applyChangeTrust(app, d1, secgateway, d1.nextSequenceNumber(), "USD",
                                         trustLineLimit);
                        applyAllowTrust(app, secgateway, d1, secgateway.nextSequenceNumber(), "USD",
                                        true);
                        applyAllowTrust(app, secgateway, d1, secgateway.nextSequenceNumber(), "IDR",
                                        true);

                        applyCreditPaymentTx(app, secgateway, d1, secIdrCur,
                                             secgateway.nextSequenceNumber(), trustLineBalance);

                        const Price usdPriceOfferD(3, 2);
                        // offer is sell 100 IDR for 150 USD; buy USD @ 1.5 =
                        // sell IRD @
                        // 0.66
                        auto offerD1 = applyCreateOffer(
                            app, delta, 0, d1, secIdrCur, secUsdCur,
                            usdPriceOfferD, 100 * assetMultiplier, d1.nextSequenceNumber());

                        SECTION("D not authorized to hold USD")
                        {
                            applyAllowTrust(app, secgateway, d1, secgateway.nextSequenceNumber(),
                                            "USD", false);
                        }
                        SECTION("D not authorized to send IDR")
                        {
                            applyAllowTrust(app, secgateway, d1, secgateway.nextSequenceNumber(),
                                            "IDR", false);
                        }

                        // setup e1
                        auto e1 = root.create("E", minBalance3 + 10000);

                        applyChangeTrust(app, e1, secgateway, e1.nextSequenceNumber(), "IDR",
                                         trustLineLimit);
                        applyChangeTrust(app, e1, secgateway, e1.nextSequenceNumber(), "USD",
                                         trustLineLimit);
                        applyAllowTrust(app, secgateway, e1, secgateway.nextSequenceNumber(), "USD",
                                        true);
                        applyAllowTrust(app, secgateway, e1, secgateway.nextSequenceNumber(), "IDR",
                                        true);

                        applyCreditPaymentTx(app, secgateway, e1, secIdrCur,
                                             secgateway.nextSequenceNumber(), trustLineBalance);

                        uint64_t offerE1 = applyCreateOffer(
                            app, delta, 0, e1, secIdrCur, secUsdCur,
                            usdPriceOfferD, 100 * assetMultiplier, e1.nextSequenceNumber());

                        // setup f1
                        auto f1 = root.create("F", minBalance3 + 10000);

                        applyChangeTrust(app, f1, secgateway, f1.nextSequenceNumber(), "IDR",
                                         trustLineLimit);
                        applyChangeTrust(app, f1, secgateway, f1.nextSequenceNumber(), "USD",
                                         trustLineLimit);
                        applyAllowTrust(app, secgateway, f1, secgateway.nextSequenceNumber(), "USD",
                                        true);
                        applyAllowTrust(app, secgateway, f1, secgateway.nextSequenceNumber(), "IDR",
                                        true);

                        applyCreditPaymentTx(app, secgateway, f1, secUsdCur,
                                             secgateway.nextSequenceNumber(), trustLineBalance);

                        // try to create an offer:
                        // it will cross with the offer from E and skip the
                        // offer from D
                        // it should still be able to buy 100 IDR / sell 150 USD

                        // offer is buy 200 IDR for 300 USD; buy IDR @ 0.66 USD
                        // -> sell USD @ 1.5 IDR
                        const Price idrPriceOfferC(2, 3);
                        auto offerF1Res = applyCreateOfferWithResult(
                            app, delta, 0, f1, secUsdCur, secIdrCur,
                            idrPriceOfferC, 300 * assetMultiplier, f1.nextSequenceNumber());
                        // offer created would be buy 100 IDR for 150 USD ; 0.66
                        REQUIRE(offerF1Res.success().offer.effect() ==
                                MANAGE_OFFER_CREATED);

                        REQUIRE(offerF1Res.success().offer.offer().amount ==
                                150 * assetMultiplier);

                        TrustFrame::pointer line;

                        // check balances

                        // D1's offer was deleted
                        REQUIRE(!loadOffer(d1, offerD1, app, false));

                        line = loadTrustLine(d1, secUsdCur, app);
                        checkAmounts(0, line->getBalance());

                        line = loadTrustLine(d1, secIdrCur, app);
                        checkAmounts(trustLineBalance, line->getBalance());

                        // E1's offer was taken
                        REQUIRE(!loadOffer(e1, offerE1, app, false));

                        line = loadTrustLine(e1, secUsdCur, app);
                        checkAmounts(line->getBalance(), 150 * assetMultiplier);

                        line = loadTrustLine(e1, secIdrCur, app);
                        checkAmounts(line->getBalance(),
                                     trustLineBalance - 100 * assetMultiplier);

                        // F1
                        line = loadTrustLine(f1, secUsdCur, app);
                        checkAmounts(line->getBalance(),
                                     trustLineBalance - 150 * assetMultiplier);

                        line = loadTrustLine(f1, secIdrCur, app);
                        checkAmounts(line->getBalance(), 100 * assetMultiplier);
                    }
                    SECTION("Creates an offer, top seller reaches limit")
                    {
                        // makes "A" only capable of holding 75 "USD"
                        applyCreditPaymentTx(
                            app, gateway, a1, usdCur, gateway.nextSequenceNumber(),
                            trustLineLimit - 75 * assetMultiplier);

                        // try to create an offer:
                        // it will cross with the offer from B fully
                        // but partially cross the offer from A
                        // it should still be able to buy 150 IDR / sell 225 USD

                        // offer is buy 200 IDR for 300 USD; buy IDR @ 0.66 USD
                        // -> sell USD @ 1.5 IDR
                        const Price idrPriceOfferC(2, 3);
                        auto offerC1Res = applyCreateOfferWithResult(
                            app, delta, 0, c1, usdCur, idrCur, idrPriceOfferC,
                            300 * assetMultiplier, c1.nextSequenceNumber());
                        // offer created would be buy 50 IDR for 75 USD ; 0.66
                        REQUIRE(offerC1Res.success().offer.effect() ==
                                MANAGE_OFFER_CREATED);

                        REQUIRE(offerC1Res.success().offer.offer().amount ==
                                75 * assetMultiplier);

                        TrustFrame::pointer line;

                        // check balances

                        // A1's offer was deleted
                        REQUIRE(!loadOffer(a1, offerA1, app, false));

                        line = loadTrustLine(a1, usdCur, app);
                        checkAmounts(trustLineLimit, line->getBalance());

                        line = loadTrustLine(a1, idrCur, app);
                        checkAmounts(trustLineBalance - 50 * assetMultiplier,
                                     line->getBalance());

                        // B1's offer was taken
                        REQUIRE(!loadOffer(b1, offerB1, app, false));

                        line = loadTrustLine(b1, usdCur, app);
                        checkAmounts(line->getBalance(), 150 * assetMultiplier);

                        line = loadTrustLine(b1, idrCur, app);
                        checkAmounts(line->getBalance(),
                                     trustLineBalance - 100 * assetMultiplier);

                        // C1
                        line = loadTrustLine(c1, usdCur, app);
                        checkAmounts(line->getBalance(),
                                     trustLineBalance - 225 * assetMultiplier);

                        line = loadTrustLine(c1, idrCur, app);
                        checkAmounts(line->getBalance(), 150 * assetMultiplier);
                    }
                }
            }
            SECTION("issuer offers")
            {
                TrustFrame::pointer line;

                SECTION("issuer creates an offer, claimed by somebody else")
                {
                    // sell 100 IDR for 90 USD
                    uint64_t gwOffer = applyCreateOffer(
                        app, delta, 0, gateway, idrCur, usdCur, Price(9, 10),
                        100 * assetMultiplier, gateway.nextSequenceNumber());

                    // fund a1 with some USD
                    applyCreditPaymentTx(app, gateway, a1, usdCur,
                                         gateway.nextSequenceNumber(), 1000 * assetMultiplier);

                    // sell USD for IDR
                    auto resA = applyCreateOfferWithResult(
                        app, delta, 0, a1, usdCur, idrCur, Price(1, 1),
                        90 * assetMultiplier, a1.nextSequenceNumber());

                    REQUIRE(resA.success().offer.effect() ==
                            MANAGE_OFFER_DELETED);

                    // gw's offer was deleted
                    REQUIRE(!loadOffer(gateway, gwOffer, app, false));

                    // check balance
                    line = loadTrustLine(a1, usdCur, app);
                    checkAmounts(910 * assetMultiplier, line->getBalance());

                    line = loadTrustLine(a1, idrCur, app);
                    checkAmounts(trustLineBalance + 100 * assetMultiplier,
                                 line->getBalance());
                }
                SECTION("issuer claims an offer from somebody else")
                {
                    auto res = applyCreateOfferWithResult(
                        app, delta, 0, gateway, usdCur, idrCur, Price(2, 3),
                        150 * assetMultiplier, gateway.nextSequenceNumber());
                    REQUIRE(res.success().offer.effect() ==
                            MANAGE_OFFER_DELETED);

                    // A's offer was deleted
                    REQUIRE(!loadOffer(a1, offerA1, app, false));

                    // check balance
                    line = loadTrustLine(a1, usdCur, app);
                    checkAmounts(150 * assetMultiplier, line->getBalance());

                    line = loadTrustLine(a1, idrCur, app);
                    checkAmounts(trustLineBalance - 100 * assetMultiplier,
                                 line->getBalance());
                }
            }
        }
    }
}
