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

    const int64_t trustLineLimit = 1000000;

    uint64_t txfee = app.getLedgerMaster().getTxFee();

    uint32_t a1_seq=1, b1_seq=1, root_seq=1, gateway_seq=1;

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 = app.getLedgerMaster().getMinBalance(2);

    Currency idrCur=makeCurrency(gateway,"IDR");
    Currency usdCur = makeCurrency(gateway, "USD");

    SECTION("account a1 does not exist")
    {
        auto txFrame = createOfferTx(a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, a1_seq);

        LedgerDelta delta;
        txFrame->apply(delta, app);
        REQUIRE(txFrame->getResultCode() == txNO_ACCOUNT);
    }

    // sets up gateway account
    applyPaymentTx(app, root, gateway, root_seq++, minBalance2);

    uint32_t txRate = (TRANSFER_RATE_DIVISOR-TRANSFER_RATE_DIVISOR/10); // 90%

    applySetOptions(app, gateway, nullptr, nullptr, &txRate, nullptr, nullptr, nullptr, gateway_seq++);

    SECTION("negative offer creation tests")
    {
        applyPaymentTx(app, root, a1, root_seq++, minBalance2 + 10000);

        // missing USD trust
        applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, a1_seq++, CreateOffer::NO_TRUST);

        applyTrust(app, a1, gateway, a1_seq++, "USD");

        // missing IDR trust
        applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, a1_seq++, CreateOffer::NO_TRUST);

        applyTrust(app, a1, gateway, a1_seq++, "IDR");

        // can't sell IDR if account doesn't have any
        applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, a1_seq++, CreateOffer::UNDERFUNDED);

        // fund a1 with some IDR
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++, trustLineLimit);

        // need sufficient STR funds to create an offer
        applyOffer(app, a1, idrCur, usdCur, OFFER_PRICE_DIVISOR, 100, a1_seq++, CreateOffer::UNDERFUNDED);

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

        applyTrust(app, a1, gateway, a1_seq++, "USD");
        applyTrust(app, a1, gateway, a1_seq++, "IDR");
        applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++, trustLineLimit);

        // create nbOffers
        std::vector<uint32_t> a1OfferSeq;

        int64_t usdPriceOfferA = OFFER_PRICE_DIVISOR*2/3;

        for (int i = 0; i < nbOffers; i++)
        {
            a1OfferSeq.push_back(a1_seq++);

            // offer is sell 100 IDR for 150 USD; buy USD @ 1.5
            applyOffer(app, a1, idrCur, usdCur, usdPriceOfferA, 100, a1OfferSeq[i]);
            REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1OfferSeq[i], offer, app.getDatabase()));

            // verifies that the offer was created as expected
            REQUIRE(offer.getPrice() == usdPriceOfferA);
            REQUIRE(offer.getAmount() == 100);
            REQUIRE(offer.getTakerGets().isoCI().currencyCode == idrCur.isoCI().currencyCode);
            REQUIRE(offer.getTakerPays().isoCI().currencyCode == usdCur.isoCI().currencyCode);
        }

        applyPaymentTx(app, root, b1, root_seq++, minBalance3 + 10000);
        applyTrust(app, b1, gateway, b1_seq++, "IDR");
        applyTrust(app, b1, gateway, b1_seq++, "USD");

        SECTION("offer that doesn't cross")
        {
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++, 20000);

            uint32_t b1OfferSeq = b1_seq++;

            // offer is sell 40 USD for 80 IDR ; sell USD @ 2
            applyOffer(app, b1, usdCur, idrCur, OFFER_PRICE_DIVISOR * 2, 40, b1OfferSeq);

            // verifies that the offer was created properly
            REQUIRE(OfferFrame::loadOffer(b1.getPublicKey(), b1OfferSeq, offer, app.getDatabase()));
            REQUIRE(offer.getPrice() == OFFER_PRICE_DIVISOR * 2);
            REQUIRE(offer.getAmount() == 40);
            REQUIRE(offer.getTakerPays().isoCI().currencyCode == idrCur.isoCI().currencyCode);
            REQUIRE(offer.getTakerGets().isoCI().currencyCode == usdCur.isoCI().currencyCode);

            // and that a1 offers were not touched
            for (auto a1Offer : a1OfferSeq)
            {
                REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer, app.getDatabase()));
                REQUIRE(offer.getPrice() == usdPriceOfferA);
                REQUIRE(offer.getAmount() == 100);
                REQUIRE(offer.getTakerPays().isoCI().currencyCode == usdCur.isoCI().currencyCode);
                REQUIRE(offer.getTakerGets().isoCI().currencyCode == idrCur.isoCI().currencyCode);
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
            applyCreditPaymentTx(app, gateway, b1, usdCur, gateway_seq++, 20000);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line, app.getDatabase()));
            int64_t b1_usd = line.getBalance();

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line, app.getDatabase()));
            int64_t b1_idr = line.getBalance();

            uint32_t b1OfferSeq = b1_seq++;

            // offer is sell 1010 USD for 505 IDR; sell USD @ 0.5
            applyOffer(app, b1, usdCur, idrCur, OFFER_PRICE_DIVISOR / 2, 1010, b1OfferSeq);

            // offer is cleared
            // TODO: offer is not cleared because of rounding issues
            //REQUIRE(!OfferFrame::loadOffer(b1.getPublicKey(), b1OfferSeq, offer, app.getDatabase()));

            for (int i = 0; i < nbOffers; i++)
            {
                int32_t a1Offer = a1OfferSeq[i];

                if (i < 15)
                {
                    // first 5 offers are taken
                    REQUIRE(!OfferFrame::loadOffer(a1.getPublicKey(), a1Offer,
                        offer, app.getDatabase()));
                }
                else
                {
                    // others are untouched
                    REQUIRE(OfferFrame::loadOffer(a1.getPublicKey(), a1Offer, offer,
                        app.getDatabase()));
                    REQUIRE(offer.getPrice() == usdPriceOfferA);
                    REQUIRE(offer.getTakerPays().isoCI().currencyCode == usdCur.isoCI().currencyCode);
                    REQUIRE(offer.getTakerGets().isoCI().currencyCode == idrCur.isoCI().currencyCode);
                    if (i == 15)
                    {
                        REQUIRE(offer.getAmount() == 70);
                    }
                    else
                    {
                        REQUIRE(offer.getAmount() == 100);
                    }
                }
            }

            // TODO: rounding issues prevent this code from working
#if 0
            // check balances
            // the USDs were sold at the (better) rate found in the original offers
            int64_t usdRecv = 1010;
            int64_t usdSend = usdRecv * 10 / 9;

            int64_t idrSend = usdRecv * 3 / 2;
            int64_t idrRecv = idrSend * 9 / 10;

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line, app.getDatabase()));
            REQUIRE(line.getBalance() == a1_usd + usdRescv);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line, app.getDatabase()));
            REQUIRE(line.getBalance() == a1_idr - idrSend);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line, app.getDatabase()));
            REQUIRE(line.getBalance() == b1_usd - usdSend);

            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line, app.getDatabase()));
            REQUIRE(line.getBalance() == b1_idr + idrRecv);
#endif

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
