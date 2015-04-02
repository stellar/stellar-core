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

    const uint64_t paymentAmount =
        (uint64_t)app.getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    // create an account
    applyPaymentTx(app, root, a1, rootSeq++, paymentAmount);

    SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

    const uint64_t morePayment = paymentAmount / 2;

    SecretKey gateway = getAccount("gate");

    const int64_t currencyMultiplier = 1000000;

    int64_t trustLineLimit = 1000000 * currencyMultiplier;

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

        // add a couple offers in the order book

        OfferFrame offer;

        const Price usdPriceOffer(3, 2);

        // b1 sells the same thing
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

        // offer is buy 150 USD for 100 IDR; buy USD @ 1.5 = sell IRD @ 0.66
        uint64_t offerC1 =
            applyCreateOffer(app, delta, 0, c1, idrCur, usdCur, usdPriceOffer,
                             100 * currencyMultiplier, c1Seq++);

        SECTION("send with path (over sendmax)")
        {
            // A1: try to send 100 IDR to B1 via USD
            // with sendMax set to 149 USD

            std::vector<Currency> path;
            path.push_back(usdCur);

            TransactionFramePtr txFrame = createCreditPaymentTx(a1, b1, idrCur, a1Seq++, 100 * currencyMultiplier, &path);
            getFirstOperation(*txFrame).body.paymentOp().sendMax = 149 * currencyMultiplier;
            reSignTransaction(*txFrame, a1);

            txFrame->apply(delta, app);

            REQUIRE(getFirstResult(*txFrame).tr().paymentResult().code() == PAYMENT_OVER_SENDMAX);
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
    const uint64_t paymentAmount =
        (uint64_t)app->getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    {
        auto ctx = app->getDatabase().captureAndLogSQL("payment");
        applyPaymentTx(*app, root, a1, rootSeq++, paymentAmount);
    }
}
