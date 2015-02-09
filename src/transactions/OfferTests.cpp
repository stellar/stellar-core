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

bool equalAmounts(int64_t a, int64_t b, int64_t maxd = 1)
{
    int64_t d = a - b;
    if (d < 0)
        d = -d;
    return (d <= maxd);
}

// Offer that doesn't cross
// Offer that crosses exactly
// Offer that takes multiple other offers and is cleared
// Offer that takes multiple other offers and remains
// Offer selling STR
// Offer buying STR
// Offer with transfer rate
// Offer for more than you have
// Offer for something you can't hold

TEST_CASE("create offer", "[tx][offers]")
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

    const int64_t currencyMultiplier = 10000000;
    const int64_t trustLineLimit = 1000000 * currencyMultiplier;


    uint64_t txfee = app.getLedgerMaster().getTxFee();

    uint32_t a1_seq=1, b1_seq=1, root_seq=1, gateway_seq=1;

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 = app.getLedgerMaster().getMinBalance(2);

    Currency idrCur=makeCurrency(gateway,"IDR");
    Currency usdCur = makeCurrency(gateway, "USD");

    SECTION("account a1 does not exist")
    {
        auto txFrame = createOfferTx(a1, idrCur, 100 * currencyMultiplier,
            usdCur, 100 * currencyMultiplier, a1_seq);

        LedgerDelta delta;
        txFrame->apply(delta, app);
        REQUIRE(txFrame->getResultCode() == txNO_ACCOUNT);
    }

    // sets up gateway account
    applyPaymentTx(app, root, gateway, root_seq++, minBalance2);

    SECTION("negative offer creation tests")
    {
        applyPaymentTx(app, root, a1, root_seq++, minBalance2 + 10000);

        // missing USD trust
        applyOffer(app, a1, idrCur, 100, usdCur, 100, a1_seq++, CreateOffer::NO_TRUST);

        applyChangeTrust(app, a1, gateway, a1_seq++, "USD", trustLineLimit);

        // missing IDR trust
        applyOffer(app, a1, idrCur, 100, usdCur, 100, a1_seq++, CreateOffer::NO_TRUST);

        applyChangeTrust(app, a1, gateway, a1_seq++, "IDR", trustLineLimit);

        // can't sell IDR if account doesn't have any
        applyOffer(app, a1, idrCur, 100, usdCur, 100, a1_seq++, CreateOffer::UNDERFUNDED);

        // fund a1 with some IDR
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++, trustLineLimit);

        // need sufficient STR funds to create an offer
        applyOffer(app, a1,
            idrCur, 100 * currencyMultiplier,
            usdCur, 100 * currencyMultiplier,
            a1_seq++, CreateOffer::UNDERFUNDED);

        // there should be no pending offer at this point in the system
        OfferFrame offer;
        REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), 5, offer, app.getDatabase()));
    }

    // minimum balance to hold
    // 2 trust lines and one offer
    const int64_t minBalance3 = app.getLedgerMaster().getMinBalance(3);

    SECTION("a1 setup properly")
    {
        OfferFrame offer;
        // fund a1 with some IDR and STR

        const int nbOffers = 22;

        const int64_t minBalanceA = app.getLedgerMaster().getMinBalance(2+nbOffers);

        applyPaymentTx(app, root, a1, root_seq++, minBalanceA + 10000);

        applyChangeTrust(app, a1, gateway, a1_seq++, "USD", trustLineLimit);
        applyChangeTrust(app, a1, gateway, a1_seq++, "IDR", trustLineLimit);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++, trustLineLimit);

        // create nbOffers
        std::vector<uint32_t> a1OfferSeq;

        for (int i = 0; i < nbOffers; i++)
        {
            a1OfferSeq.push_back(a1_seq++);

            // offer is sell 100 IDR for 150 USD; buy USD @ 1.5
            applyOffer(app, a1,
                idrCur, 100 * currencyMultiplier,
                usdCur, 150 * currencyMultiplier,
                a1OfferSeq[i]);
            REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1OfferSeq[i], offer, app.getDatabase()));

            // verifies that the offer was created as expected
            REQUIRE(offer.getSell().isoCI().currencyCode == idrCur.isoCI().currencyCode);
            REQUIRE(offer.getSellAmount() == 100 * currencyMultiplier);

            REQUIRE(offer.getBuy().isoCI().currencyCode == usdCur.isoCI().currencyCode);
            REQUIRE(offer.getBuyAmount() == 150 * currencyMultiplier);
        }

        applyPaymentTx(app, root, b1, root_seq++, minBalance3 + 10000);
        applyChangeTrust(app, b1, gateway, b1_seq++, "IDR", trustLineLimit);
        applyChangeTrust(app, b1, gateway, b1_seq++, "USD", trustLineLimit);

        SECTION("offer that doesn't cross")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++, 20000 * currencyMultiplier);

            uint32_t b1OfferSeq = b1_seq++;

            // offer is sell 40 USD for 80 IDR ; sell USD @ 2
            applyOffer(app, b1,
                usdCur, 40 * currencyMultiplier,
                idrCur, 80 * currencyMultiplier,
                b1OfferSeq);

            // verifies that the offer was created properly
            REQUIRE(OfferFrame::loadOffer(b1.getPublicKey(), b1OfferSeq, offer, app.getDatabase()));

            REQUIRE(offer.getSell().isoCI().currencyCode == usdCur.isoCI().currencyCode);
            REQUIRE(offer.getSellAmount() == 40 * currencyMultiplier);

            REQUIRE(offer.getBuy().isoCI().currencyCode == idrCur.isoCI().currencyCode);
            REQUIRE(offer.getBuyAmount() == 80 * currencyMultiplier);

            // and that a1 offers were not touched
            for (auto a1Offer : a1OfferSeq)
            {
                REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer, app.getDatabase()));
                REQUIRE(offer.getSell().isoCI().currencyCode == idrCur.isoCI().currencyCode);
                REQUIRE(offer.getSellAmount() == 100 * currencyMultiplier);

                REQUIRE(offer.getBuy().isoCI().currencyCode == usdCur.isoCI().currencyCode);
                REQUIRE(offer.getBuyAmount() == 150 * currencyMultiplier);
            }
        }

        // Crossing your own offer

        // Too small offers
        // Trying to extract value from an offer
        // Unfunded offer getting cleaned up

        // Offer that crosses with some left in the new offer
        // Offer that crosses with none left in the new offer
        // Offer that crosses and takes out both

        // Offer that crosses exactly

        TrustFrame line;
        REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line, app.getDatabase()));
        int64_t a1_usd = line.getBalance();

        REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line, app.getDatabase()));
        int64_t a1_idr = line.getBalance();

        SECTION("Offer that takes multiple other offers and is cleared")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++, 20000 * currencyMultiplier);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line, app.getDatabase()));
            int64_t b1_usd = line.getBalance();

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line, app.getDatabase()));
            int64_t b1_idr = line.getBalance();

            uint32_t b1OfferSeq = b1_seq++;

            // offer is sell 1010 USD for 505 IDR; sell USD @ 0.5
            applyOffer(app, b1,
                usdCur, 1010 * currencyMultiplier,
                idrCur, 505 * currencyMultiplier,
                b1OfferSeq);

            // offer is cleared
            REQUIRE(!OfferFrame::loadOffer(b1.getPublicKey(), b1OfferSeq, offer, app.getDatabase()));

            // Offers are: sell 100 IDR for 150 USD; sell IRD @ 0.66 -> buy USD @ 1.5
            // first 6 offers get taken for 6*150=900 USD, gets 600 IDR in return
            // offer #7 : has 110 USD available -> can claim partial offer 100*110/150 = 73 ; 27
            // 8 .. untouched

            for (int i = 0; i < nbOffers; i++)
            {
                int32_t a1Offer = a1OfferSeq[i];

                if (i < 6)
                {
                    // first 6 offers are taken
                    REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                        offer, app.getDatabase()));
                }
                else
                {
                    REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer,
                        app.getDatabase()));
                    REQUIRE(offer.getSell().isoCI().currencyCode == idrCur.isoCI().currencyCode);
                    REQUIRE(offer.getBuy().isoCI().currencyCode == usdCur.isoCI().currencyCode);
                    if (i == 6)
                    {
                        REQUIRE(equalAmounts(offer.getBuyAmount(), (7*150-1010)*currencyMultiplier));
                    }
                    else
                    {
                        // others are untouched
                        REQUIRE(offer.getSellAmount() == 100 * currencyMultiplier);
                        REQUIRE(offer.getBuyAmount() == 150 * currencyMultiplier);
                    }
                }
            }

            // check balances
            // the USDs were sold at the (better) rate found in the original offers
            int64_t usdRecv = 1010 * currencyMultiplier;

            int64_t idrSend = bigDivide(usdRecv, 2, 3);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), a1_usd + usdRecv));

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), a1_idr - idrSend));

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), b1_usd - usdRecv));

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), b1_idr + idrSend));

        }

        SECTION("Offers being created to pick on a single offer")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++, 20000 * currencyMultiplier);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line, app.getDatabase()));
            int64_t b1_usd = line.getBalance();

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line, app.getDatabase()));
            int64_t b1_idr = line.getBalance();

            uint32_t b1OfferSeq = b1_seq++;

            for (int j = 0; j < 10; j++)
            {
                // offer is sell 1 USD for 0.5 IDR; sell USD @ 0.5
                applyOffer(app, b1,
                    usdCur, 1 * currencyMultiplier,
                    idrCur, bigDivide(5, currencyMultiplier, 10),
                    b1OfferSeq++);
                REQUIRE(!OfferFrame::loadOffer(b1.getPublicKey(), b1OfferSeq, offer, app.getDatabase()));
            }

            for (int i = 0; i < nbOffers; i++)
            {
                int32_t a1Offer = a1OfferSeq[i];

                REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer,
                    app.getDatabase()));
                REQUIRE(offer.getSell().isoCI().currencyCode == idrCur.isoCI().currencyCode);
                REQUIRE(offer.getBuy().isoCI().currencyCode == usdCur.isoCI().currencyCode);
                if (i == 0)
                {
                    // TODO: is there a way to avoid 1 stroop deviation per transaction
                    // 10 transactions -> 10 stroops distance
                    REQUIRE(equalAmounts(offer.getBuyAmount(), (150 - 10)*currencyMultiplier, 10));
                }
                else
                {
                    // others are untouched
                    REQUIRE(offer.getSellAmount() == 100 * currencyMultiplier);
                    REQUIRE(offer.getBuyAmount() == 150 * currencyMultiplier);
                }
            }

            // check balances
            // the USDs were sold at the (better) rate found in the original offers
            int64_t usdRecv = 10 * currencyMultiplier;

            int64_t idrSend = bigDivide(usdRecv, 2, 3);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), a1_usd + usdRecv, 10));

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), a1_idr - idrSend, 10));

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), b1_usd - usdRecv, 10));

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line, app.getDatabase()));
            REQUIRE(equalAmounts(line.getBalance(), b1_idr + idrSend, 10));
        }

        // Offer that takes multiple other offers and remains
        // Offer selling STR
        // Offer buying STR
        // Offer with transfer rate
        // Offer for more than you have
        // Offer for something you can't hold
        // Passive offer
    }
}
