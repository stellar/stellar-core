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
#include "ledger/LedgerDelta.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// checks that b-maxd <= a <= b
// bias towards seller means
//    * amount left in an offer should be higher than the exact calculation
//    * amount received by a seller should be higher than the exact calculation
void
checkAmounts(int64_t a, int64_t b, int64_t maxd = 1)
{
    int64_t d = b - maxd;
    REQUIRE(a >= d);
    REQUIRE(a <= b);
}

// Offer that takes multiple other offers and remains
// Offer selling STR
// Offer buying STR
// Offer with transfer rate
// Offer for more than you have
// Offer for something you can't hold
// Offer with line full (both accounts)

TEST_CASE("create offer", "[tx][offers]")
{
    Config const& cfg = getTestConfig();
    Config cfg2(cfg);
    // cfg2.DATABASE = "sqlite3://test.db";
    // cfg2.DATABASE = "postgresql://dbmaster:-island-@localhost/hayashi";

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg2);
    Application& app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");
    SecretKey gateway = getAccount("gate");

    const int64_t currencyMultiplier = 1000000;

    int64_t trustLineLimit = 1000000 * currencyMultiplier;

    int64_t txfee = app.getLedgerMaster().getTxFee();

    SequenceNumber root_seq = getAccountSeqNum(root, app) + 1;
    ;

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        app.getLedgerMaster().getMinBalance(2) + 10 * txfee;

    Currency idrCur = makeCurrency(gateway, "IDR");
    Currency usdCur = makeCurrency(gateway, "USD");

    const Price oneone(1, 1);

    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

    SECTION("account a1 does not exist")
    {
        auto txFrame = createOfferOp(a1, idrCur, usdCur, oneone, 100, 1);

        txFrame->apply(delta, app);
        REQUIRE(txFrame->getResultCode() == txNO_ACCOUNT);
    }

    // sets up gateway account
    applyPaymentTx(app, root, gateway, root_seq++, minBalance2);
    SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

    SECTION("negative offer creation tests")
    {
        applyPaymentTx(app, root, a1, root_seq++, minBalance2 + 10000);
        SequenceNumber a1_seq = getAccountSeqNum(a1, app) + 1;

        // missing USD trust
        applyCreateOfferWithResult(app, delta, a1, idrCur, usdCur, oneone, 100,
                                   a1_seq++, CreateOffer::NO_TRUST);

        applyChangeTrust(app, a1, gateway, a1_seq++, "USD", trustLineLimit);

        // missing IDR trust
        applyCreateOfferWithResult(app, delta, a1, idrCur, usdCur, oneone, 100,
                                   a1_seq++, CreateOffer::NO_TRUST);

        applyChangeTrust(app, a1, gateway, a1_seq++, "IDR", trustLineLimit);

        // can't sell IDR if account doesn't have any
        applyCreateOfferWithResult(app, delta, a1, idrCur, usdCur, oneone, 100,
                                   a1_seq++, CreateOffer::UNDERFUNDED);

        // fund a1 with some IDR
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++,
                             trustLineLimit);

        // need sufficient STR funds to create an offer
        applyCreateOfferWithResult(app, delta, a1, idrCur, usdCur, oneone, 100,
                                   a1_seq++, CreateOffer::UNDERFUNDED);

        // there should be no pending offer at this point in the system
        OfferFrame offer;
        REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), 5, offer,
                                       app.getDatabase()));
    }

    // minimum balance to hold
    // 2 trust lines and one offer
    const int64_t minBalance3 = app.getLedgerMaster().getMinBalance(3);

    SECTION("a1 setup properly")
    {
        OfferFrame offer;
        // fund a1 with some IDR and STR

        const int nbOffers = 22;

        const int64_t minBalanceA =
            app.getLedgerMaster().getMinBalance(3 + nbOffers);

        applyPaymentTx(app, root, a1, root_seq++, minBalanceA + 10000);
        SequenceNumber a1_seq = getAccountSeqNum(a1, app) + 1;

        applyChangeTrust(app, a1, gateway, a1_seq++, "USD", trustLineLimit);
        applyChangeTrust(app, a1, gateway, a1_seq++, "IDR", trustLineLimit);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++,
                             trustLineLimit);

        // create nbOffers
        std::vector<uint64_t> a1OfferID;
        const Price usdPriceOfferA(3, 2);

        for (int i = 0; i < nbOffers; i++)
        {

            // offer is sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy USD @
            // 1.5
            uint64_t newOfferID =
                applyCreateOffer(app, delta, a1, idrCur, usdCur, usdPriceOfferA,
                                 100 * currencyMultiplier, a1_seq++);

            REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), newOfferID, offer,
                                          app.getDatabase()));

            a1OfferID.push_back(newOfferID);

            // verifies that the offer was created as expected
            REQUIRE(offer.getPrice() == usdPriceOfferA);
            REQUIRE(offer.getAmount() == 100 * currencyMultiplier);
            REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                    idrCur.isoCI().currencyCode);
            REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                    usdCur.isoCI().currencyCode);
        }

        applyPaymentTx(app, root, b1, root_seq++, minBalance3 + 10000);
        SequenceNumber b1_seq = getAccountSeqNum(b1, app) + 1;
        applyChangeTrust(app, b1, gateway, b1_seq++, "IDR", trustLineLimit);
        applyChangeTrust(app, b1, gateway, b1_seq++, "USD", trustLineLimit);

        const Price twoone(2, 1);

        SECTION("offer that doesn't cross")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++,
                                 20000 * currencyMultiplier);

            // offer is sell 40 USD for 80 IDR ; sell USD @ 2

            uint64_t offerID =
                applyCreateOffer(app, delta, b1, usdCur, idrCur, twoone,
                                 40 * currencyMultiplier, b1_seq++);

            // verifies that the offer was created properly
            REQUIRE(OfferFrame::loadOffer(b1.getPublicKey(), offerID, offer,
                                          app.getDatabase()));
            REQUIRE(offer.getPrice() == twoone);
            REQUIRE(offer.getAmount() == 40 * currencyMultiplier);
            REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                    idrCur.isoCI().currencyCode);
            REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                    usdCur.isoCI().currencyCode);

            // and that a1 offers were not touched
            for (auto a1Offer : a1OfferID)
            {
                REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer,
                                              app.getDatabase()));
                REQUIRE(offer.getPrice() == usdPriceOfferA);
                REQUIRE(offer.getAmount() == 100 * currencyMultiplier);
                REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                        usdCur.isoCI().currencyCode);
                REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                        idrCur.isoCI().currencyCode);
            }
        }

        SECTION("Offer crossing own offer")
        {
            applyCreditPaymentTx(app, gateway, a1, usdCur, gateway_seq++,
                                 20000 * currencyMultiplier);

            // offer is sell 150 USD for 100 USD; sell USD @ 1.5 / buy IRD @
            // 0.66
            Price exactCross(usdPriceOfferA.d, usdPriceOfferA.n);

            uint64_t beforeID = delta.getHeaderFrame().getLastGeneratedID();
            applyCreateOfferWithResult(app, delta, a1, usdCur, idrCur,
                                       exactCross, 150 * currencyMultiplier,
                                       a1_seq++, CreateOffer::CROSS_SELF);
            REQUIRE(beforeID == delta.getHeaderFrame().getLastGeneratedID());

            for (auto a1Offer : a1OfferID)
            {
                REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer,
                                              app.getDatabase()));
                REQUIRE(offer.getPrice() == usdPriceOfferA);
                REQUIRE(offer.getAmount() == 100 * currencyMultiplier);
                REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                        usdCur.isoCI().currencyCode);
                REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                        idrCur.isoCI().currencyCode);
            }
        }

        SECTION("Offer that crosses exactly")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++,
                                 20000 * currencyMultiplier);

            // offer is sell 150 USD for 100 USD; sell USD @ 1.5 / buy IRD @
            // 0.66
            Price exactCross(usdPriceOfferA.d, usdPriceOfferA.n);

            uint64_t expectedID =
                delta.getHeaderFrame().getLastGeneratedID() + 1;
            auto const& res = applyCreateOfferWithResult(
                app, delta, b1, usdCur, idrCur, exactCross,
                150 * currencyMultiplier, b1_seq++);

            REQUIRE(res.success().offer.effect() == CreateOffer::EMPTY);

            // verifies that the offer was not created
            REQUIRE(!OfferFrame::loadOffer(b1.getPublicKey(), expectedID, offer,
                                           app.getDatabase()));

            // and the state of a1 offers
            for (int i = 0; i < nbOffers; i++)
            {
                uint64_t a1Offer = a1OfferID[i];

                if (i == 0)
                {
                    // first offer was taken
                    REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                                                   offer, app.getDatabase()));
                }
                else
                {
                    REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                                                  offer, app.getDatabase()));
                    REQUIRE(offer.getPrice() == usdPriceOfferA);
                    REQUIRE(offer.getAmount() == 100 * currencyMultiplier);
                    REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                            usdCur.isoCI().currencyCode);
                    REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                            idrCur.isoCI().currencyCode);
                }
            }
        }

        TrustFrame line;
        REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line,
                                          app.getDatabase()));
        int64_t a1_usd = line.getBalance();

        REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                          app.getDatabase()));
        int64_t a1_idr = line.getBalance();

        const Price onetwo(1, 2);

        SECTION("Offer that takes multiple other offers and is cleared")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++,
                                 20000 * currencyMultiplier);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            int64_t b1_usd = line.getBalance();

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            int64_t b1_idr = line.getBalance();

            uint64_t expectedID =
                delta.getHeaderFrame().getLastGeneratedID() + 1;
            // offer is sell 1010 USD for 505 IDR; sell USD @ 0.5
            auto const& res = applyCreateOfferWithResult(
                app, delta, b1, usdCur, idrCur, onetwo,
                1010 * currencyMultiplier, b1_seq++);

            REQUIRE(res.success().offer.effect() == CreateOffer::EMPTY);
            // verify that the offer was not created
            REQUIRE(!OfferFrame::loadOffer(b1.getPublicKey(), expectedID, offer,
                                           app.getDatabase()));

            // Offers are: sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy USD
            // @ 1.5
            // first 6 offers get taken for 6*150=900 USD, gets 600 IDR in
            // return
            // offer #7 : has 110 USD available
            //    -> can claim partial offer 100*110/150 = 73.333 ; -> 26.66666
            //    left
            // 8 .. untouched
            // the USDs were sold at the (better) rate found in the original
            // offers
            int64_t usdRecv = 1010 * currencyMultiplier;

            int64_t idrSend = bigDivide(usdRecv, 2, 3);

            for (int i = 0; i < nbOffers; i++)
            {
                uint64_t a1Offer = a1OfferID[i];

                if (i < 6)
                {
                    // first 6 offers are taken
                    REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                                                   offer, app.getDatabase()));
                }
                else
                {
                    // others are untouched
                    REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                                                  offer, app.getDatabase()));
                    REQUIRE(offer.getPrice() == usdPriceOfferA);
                    REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                            usdCur.isoCI().currencyCode);
                    REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                            idrCur.isoCI().currencyCode);
                    if (i == 6)
                    {
                        int64_t expected =
                            100 * currencyMultiplier -
                            (idrSend - 6 * 100 * currencyMultiplier);
                        checkAmounts(expected, offer.getAmount());
                    }
                    else
                    {
                        REQUIRE(offer.getAmount() == 100 * currencyMultiplier);
                    }
                }
            }

            // check balances
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(a1_usd + usdRecv, line.getBalance());

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(a1_idr - idrSend, line.getBalance());

            // buyer may have paid a bit more to cross offers
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), b1_usd - usdRecv);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), b1_idr + idrSend);
        }

        SECTION("Trying to extract value from an offer")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++,
                                 20000 * currencyMultiplier);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            int64_t b1_usd = line.getBalance();

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            int64_t b1_idr = line.getBalance();

            // the USDs were sold at the (better) rate found in the original
            // offers
            int64_t usdRecv = 10 * currencyMultiplier;

            int64_t idrSend = bigDivide(usdRecv, 2, 3);

            for (int j = 0; j < 10; j++)
            {
                // offer is sell 1 USD for 0.5 IDR; sell USD @ 0.5

                uint64_t wouldCreateID =
                    delta.getHeaderFrame().getLastGeneratedID() + 1;
                auto const& res = applyCreateOfferWithResult(
                    app, delta, b1, usdCur, idrCur, onetwo,
                    1 * currencyMultiplier, b1_seq++);

                REQUIRE(res.success().offer.effect() == CreateOffer::EMPTY);
                REQUIRE(!OfferFrame::loadOffer(b1.getPublicKey(), wouldCreateID,
                                               offer, app.getDatabase()));
            }

            for (int i = 0; i < nbOffers; i++)
            {
                uint64_t a1Offer = a1OfferID[i];

                REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer,
                                              app.getDatabase()));

                REQUIRE(offer.getTakerPays().isoCI().currencyCode ==
                        usdCur.isoCI().currencyCode);
                REQUIRE(offer.getTakerGets().isoCI().currencyCode ==
                        idrCur.isoCI().currencyCode);

                if (i == 0)
                {
                    int64_t expected = 100 * currencyMultiplier - idrSend;
                    checkAmounts(expected, offer.getAmount(), 10);
                }
                else
                {
                    REQUIRE(offer.getAmount() == 100 * currencyMultiplier);
                }
            }

            // check balances

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(a1_usd + usdRecv, line.getBalance(), 10);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(a1_idr - idrSend, line.getBalance(), 10);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), b1_usd - usdRecv, 10);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), b1_idr + idrSend, 10);
        }

        SECTION("Offer that takes multiple other offers and remains")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++,
                                 20000 * currencyMultiplier);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            int64_t b1_usd = line.getBalance();

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            int64_t b1_idr = line.getBalance();

            // inject also an offer that should get cleaned up
            uint64_t cOfferID = 0;
            SecretKey c1 = getAccount("C");
            {
                applyPaymentTx(app, root, c1, root_seq++, minBalance3 + 10000);
                SequenceNumber c1_seq = getAccountSeqNum(c1, app) + 1;
                applyChangeTrust(app, c1, gateway, c1_seq++, "IDR",
                                 trustLineLimit);
                applyChangeTrust(app, c1, gateway, c1_seq++, "USD",
                                 trustLineLimit);
                applyCreditPaymentTx(app, gateway, c1, idrCur, gateway_seq++,
                                     20000 * currencyMultiplier);

                // matches the offer from A
                cOfferID = applyCreateOffer(app, delta, c1, idrCur, usdCur,
                                            usdPriceOfferA,
                                            100 * currencyMultiplier, c1_seq++);
                // drain account
                applyCreditPaymentTx(app, c1, gateway, idrCur, c1_seq++,
                                     20000 * currencyMultiplier);
                // offer should still be there
                REQUIRE(OfferFrame::loadOffer(c1.getPublicKey(), cOfferID,
                                              offer, app.getDatabase()));
            }

            // offer is sell 10000 USD for 5000 IDR; sell USD @ 0.5

            int64_t usdBalanceForSale = 10000 * currencyMultiplier;
            uint64_t offerID =
                applyCreateOffer(app, delta, b1, usdCur, idrCur, onetwo,
                                 usdBalanceForSale, b1_seq++);

            REQUIRE(OfferFrame::loadOffer(b1.getPublicKey(), offerID, offer,
                                          app.getDatabase()));

            // Offers are: sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy USD
            // @ 1.5
            int64_t usdRecv = 150 * currencyMultiplier * nbOffers;
            int64_t idrSend = bigDivide(usdRecv, 2, 3);

            int64_t expected = usdBalanceForSale - usdRecv;

            checkAmounts(expected, offer.getAmount());

            // check that the bogus offer was cleared
            REQUIRE(!OfferFrame::loadOffer(c1.getPublicKey(), cOfferID, offer,
                                           app.getDatabase()));

            for (int i = 0; i < nbOffers; i++)
            {
                uint64_t a1Offer = a1OfferID[i];
                REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                                               offer, app.getDatabase()));
            }

            // check balances
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(a1_usd + usdRecv, line.getBalance());

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(a1_idr - idrSend, line.getBalance());

            // buyer may have paid a bit more to cross offers
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), b1_usd - usdRecv);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), b1_idr + idrSend);
        }

        // Offer selling STR
        // Offer buying STR
        // Offer for more than you have
        // Offer for something you can't hold
        // Passive offer
    }
}
