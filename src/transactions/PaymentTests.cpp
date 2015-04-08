// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "main/Application.h"
#include "util/Timer.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// *XLM Payment
// *Credit Payment
// XLM -> Credit Payment
// Credit -> XLM Payment
// Credit -> XLM -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx][payment]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    Currency xlmCur;

    int64_t txfee = app.getLedgerManager().getTxFee();

    const int64_t paymentAmount =
        app.getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    // create an account
    applyPaymentTx(app, root, a1, rootSeq++, paymentAmount);

    SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

    const int64_t morePayment = paymentAmount / 2;

    SecretKey gateway = getAccount("gate");

    const int64_t currencyMultiplier = 1000000;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000 * currencyMultiplier;

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        app.getLedgerManager().getMinBalance(2) + 10 * txfee;

    // minimum balance necessary to hold 2 trust lines and an offer
    const int64_t minBalance3 =
        app.getLedgerManager().getMinBalance(3) + 10 * txfee;

    Currency idrCur = makeCurrency(gateway, "IDR");
    Currency usdCur = makeCurrency(gateway, "USD");

    // sets up gateway account
    const int64_t gatewayPayment = minBalance2 + morePayment;
    applyPaymentTx(app, root, gateway, rootSeq++, gatewayPayment);
    SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

    AccountFrame a1Account, rootAccount;
    REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount,
                                      app.getDatabase()));
    REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account,
                                      app.getDatabase()));
    REQUIRE(rootAccount.getMasterWeight() == 1);
    REQUIRE(rootAccount.getHighThreshold() == 0);
    REQUIRE(rootAccount.getLowThreshold() == 0);
    REQUIRE(rootAccount.getMediumThreshold() == 0);
    REQUIRE(a1Account.getBalance() == paymentAmount);
    REQUIRE(a1Account.getMasterWeight() == 1);
    REQUIRE(a1Account.getHighThreshold() == 0);
    REQUIRE(a1Account.getLowThreshold() == 0);
    REQUIRE(a1Account.getMediumThreshold() == 0);
    // root did 2 transactions at this point
    REQUIRE(rootAccount.getBalance() ==
            (100000000000000000 - paymentAmount - gatewayPayment - txfee * 2));

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    SECTION("send XLM to an existing account")
    {
        applyPaymentTx(app, root, a1, rootSeq++, morePayment);

        AccountFrame a1Account2, rootAccount2;
        REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount2,
                                          app.getDatabase()));
        REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account2,
                                          app.getDatabase()));
        REQUIRE(a1Account2.getBalance() ==
                a1Account.getBalance() + morePayment);

        // root did 2 transactions at this point
        REQUIRE(rootAccount2.getBalance() ==
                (rootAccount.getBalance() - morePayment - txfee));
    }

    SECTION("send to self")
    {
        applyPaymentTx(app, root, root, rootSeq++, morePayment);

        AccountFrame rootAccount2;
        REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount2,
                                          app.getDatabase()));
        REQUIRE(rootAccount2.getBalance() ==
                (rootAccount.getBalance() - txfee));
    }

    SECTION("send too little XLM to new account (below reserve)")
    {
        applyPaymentTx(
            app, root, b1, rootSeq++,
            app.getLedgerManager().getCurrentLedgerHeader().baseReserve - 1,
            PAYMENT_LOW_RESERVE);
    }
    SECTION("simple credit")
    {
        SECTION("credit sent to new account (no account error)")
        {
            applyCreditPaymentTx(app, gateway, b1, idrCur, gateway_seq++, 100,
                                 PAYMENT_NO_DESTINATION);
        }

        // actual sendcredit
        SECTION("credit payment with no trust")
        {
            applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++, 100,
                                 PAYMENT_NO_TRUST);
        }

        SECTION("with trust")
        {
            applyChangeTrust(app, a1, gateway, a1Seq++, "IDR", 1000);
            applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++, 100);

            TrustFrame line;
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 100);

            // create b1 account
            applyPaymentTx(app, root, b1, rootSeq++, paymentAmount);

            SequenceNumber b1Seq = getAccountSeqNum(b1, app) + 1;

            applyChangeTrust(app, b1, gateway, b1Seq++, "IDR", 100);

            // first, send 40 from a1 to b1
            applyCreditPaymentTx(app, a1, b1, idrCur, a1Seq++, 40);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 60);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 40);

            // then, send back to the gateway
            // the gateway does not have a trust line as it's the issuer
            applyCreditPaymentTx(app, b1, gateway, idrCur, b1Seq++, 40);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 0);
        }
    }
    SECTION("payment through path")
    {
        SECTION("send XLM with path (not enough offers)")
        {
            std::vector<Currency> path;
            path.push_back(idrCur);

            applyCreditPaymentTx(app, gateway, a1, xlmCur, gateway_seq++,
                                 morePayment, PAYMENT_TOO_FEW_OFFERS, &path);
        }

        // setup a1
        applyChangeTrust(app, a1, gateway, a1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, a1, gateway, a1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, gateway, a1, usdCur, gateway_seq++,
                             trustLineStartingBalance);

        // add a couple offers in the order book

        OfferFrame offer;

        const Price usdPriceOffer(2, 1);

        // offer is sell 100 IDR for 200 USD ; buy USD @ 2.0 = sell IRD @ 0.5
        applyPaymentTx(app, root, b1, rootSeq++, minBalance3 + 10000);
        SequenceNumber b1Seq = getAccountSeqNum(b1, app) + 1;
        applyChangeTrust(app, b1, gateway, b1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, b1, gateway, b1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, gateway, b1, idrCur, gateway_seq++,
                             trustLineStartingBalance);

        uint64_t offerB1 =
            applyCreateOffer(app, delta, 0, b1, idrCur, usdCur, usdPriceOffer,
                             100 * currencyMultiplier, b1Seq++);

        // setup "c1"
        SecretKey c1 = getAccount("C");

        applyPaymentTx(app, root, c1, rootSeq++, minBalance3 + 10000);
        SequenceNumber c1Seq = getAccountSeqNum(c1, app) + 1;

        applyChangeTrust(app, c1, gateway, c1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, c1, gateway, c1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, gateway, c1, idrCur, gateway_seq++,
                             trustLineStartingBalance);

        // offer is sell 100 IDR for 150 USD ; buy USD @ 1.5 = sell IRD @ 0.66
        uint64_t offerC1 =
            applyCreateOffer(app, delta, 0, c1, idrCur, usdCur, Price(3, 2),
                             100 * currencyMultiplier, c1Seq++);

        // at this point:
        // a1 holds (0, IDR) (trustLineStartingBalance, USD)
        // b1 holds (trustLineStartingBalance, IDR) (0, USD)
        // c1 holds (trustLineStartingBalance, IDR) (0, USD)
        SECTION("send with path (over sendmax)")
        {
            // A1: try to send 100 IDR to B1 via USD
            // with sendMax set to 149 USD

            std::vector<Currency> path;
            path.push_back(usdCur);

            TransactionFramePtr txFrame = createCreditPaymentTx(
                a1, b1, idrCur, a1Seq++, 100 * currencyMultiplier, &path);
            getFirstOperation(*txFrame).body.paymentOp().sendMax =
                149 * currencyMultiplier;
            reSignTransaction(*txFrame, a1);

            txFrame->apply(delta, app);

            REQUIRE(getFirstResult(*txFrame).tr().paymentResult().code() ==
                    PAYMENT_OVER_SENDMAX);
        }

        SECTION("send with path (success)")
        {
            // A1: try to send 125 IDR to B1 via USD
            // should cost 150 (C's offer taken entirely) +
            //  50 (1/4 of B's offer)=200 USD

            std::vector<Currency> path;
            path.push_back(usdCur);

            auto res = applyCreditPaymentTx(app, a1, b1, idrCur, a1Seq++,
                                            125 * currencyMultiplier,
                                            PAYMENT_SUCCESS_MULTI, &path);

            auto& multi = res.multi();

            REQUIRE(multi.offers.size() == 2);

            TrustFrame line;

            // C1
            // offer was taken
            REQUIRE(multi.offers[0].offerID == offerC1);
            REQUIRE(!OfferFrame::loadOffer(c1.getPublicKey(), offerC1, offer,
                                           app.getDatabase()));
            REQUIRE(TrustFrame::loadTrustLine(c1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(),
                         trustLineStartingBalance - 100 * currencyMultiplier);
            REQUIRE(TrustFrame::loadTrustLine(c1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), 150 * currencyMultiplier);

            // B1
            auto const& b1Res = multi.offers[1];
            REQUIRE(b1Res.offerID == offerB1);
            REQUIRE(OfferFrame::loadOffer(b1.getPublicKey(), offerB1, offer,
                                          app.getDatabase()));
            OfferEntry const& oe = offer.getOffer();
            REQUIRE(b1Res.offerOwner == b1.getPublicKey());
            checkAmounts(b1Res.amountClaimed, 25 * currencyMultiplier);
            checkAmounts(oe.amount, 75 * currencyMultiplier);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            // 125 where sent, 25 were consumed by B's offer
            checkAmounts(line.getBalance(),
                         trustLineStartingBalance +
                             (125 - 25) * currencyMultiplier);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), 50 * currencyMultiplier);

            // A1
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), 0);
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(),
                         trustLineStartingBalance - 200 * currencyMultiplier);
        }
        SECTION("send with path (offer participant reaching limit)")
        {
            // make it such that C can only receive 120 USD (4/5th of offerC)
            applyChangeTrust(app, c1, gateway, c1Seq++, "USD",
                             120 * currencyMultiplier);

            // A1: try to send 105 IDR to B1 via USD
            // cost 120 (C's offer maxed out at 4/5th of published amount)
            //  50 (1/4 of B's offer)=170 USD

            std::vector<Currency> path;
            path.push_back(usdCur);

            auto res = applyCreditPaymentTx(app, a1, b1, idrCur, a1Seq++,
                                            105 * currencyMultiplier,
                                            PAYMENT_SUCCESS_MULTI, &path);

            auto& multi = res.multi();

            REQUIRE(multi.offers.size() == 2);

            TrustFrame line;

            // C1
            // offer was taken
            REQUIRE(multi.offers[0].offerID == offerC1);
            REQUIRE(!OfferFrame::loadOffer(c1.getPublicKey(), offerC1, offer,
                                           app.getDatabase()));
            REQUIRE(TrustFrame::loadTrustLine(c1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(),
                         trustLineStartingBalance - 80 * currencyMultiplier);
            REQUIRE(TrustFrame::loadTrustLine(c1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), line.getTrustLine().limit);

            // B1
            auto const& b1Res = multi.offers[1];
            REQUIRE(b1Res.offerID == offerB1);
            REQUIRE(OfferFrame::loadOffer(b1.getPublicKey(), offerB1, offer,
                                          app.getDatabase()));
            OfferEntry const& oe = offer.getOffer();
            REQUIRE(b1Res.offerOwner == b1.getPublicKey());
            checkAmounts(b1Res.amountClaimed, 25 * currencyMultiplier);
            checkAmounts(oe.amount, 75 * currencyMultiplier);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            // 105 where sent, 25 were consumed by B's offer
            checkAmounts(line.getBalance(),
                         trustLineStartingBalance +
                             (105 - 25) * currencyMultiplier);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), 50 * currencyMultiplier);

            // A1
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(), 0);
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), usdCur, line,
                                              app.getDatabase()));
            checkAmounts(line.getBalance(),
                         trustLineStartingBalance - 170 * currencyMultiplier);
        }
        SECTION("issuer large amounts")
        {
            applyChangeTrust(app, a1, gateway, a1Seq++, "IDR", INT64_MAX);
            applyCreditPaymentTx(app, gateway, a1, idrCur, gateway_seq++,
                                 INT64_MAX);
            TrustFrame line;
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == INT64_MAX);

            // send it all back
            applyCreditPaymentTx(app, a1, gateway, idrCur, a1Seq++, INT64_MAX);
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), idrCur, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 0);

            std::vector<TrustFrame> gwLines;
            TrustFrame::loadLines(gateway.getPublicKey(), gwLines,
                                  app.getDatabase());
            REQUIRE(gwLines.size() == 0);
        }
    }
}

TEST_CASE("single payment tx SQL", "[singlesql][paymentsql][hide]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    mode = Config::TESTDB_TCP_LOCALHOST_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    int64_t txfee = app->getLedgerManager().getTxFee();
    const int64_t paymentAmount =
        app->getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    {
        auto ctx = app->getDatabase().captureAndLogSQL("payment");
        applyPaymentTx(*app, root, a1, rootSeq++, paymentAmount);
    }
}
