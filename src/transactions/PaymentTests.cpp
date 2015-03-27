// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
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
    Config cfg2(cfg);
    // cfg2.DATABASE = "sqlite3://test.db";
    // cfg2.DATABASE = "postgresql://dbmaster:-island-@localhost/stellar-core";

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg2);
    Application& app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    int64_t txfee = app.getLedgerManager().getTxFee();

    const uint64_t paymentAmount =
        (uint64_t)app.getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    // create an account
    applyPaymentTx(app, root, a1, rootSeq++, paymentAmount);

    SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

    AccountFrame a1Account, rootAccount;
    REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount,
                                      app.getDatabase()));
    REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account,
                                      app.getDatabase()));
    REQUIRE(rootAccount.getMasterWeight() == 1);
    REQUIRE(rootAccount.getHighThreshold() == 0);
    REQUIRE(rootAccount.getLowThreshold() == 0);
    REQUIRE(rootAccount.getMidThreshold() == 0);
    REQUIRE(a1Account.getBalance() == paymentAmount);
    REQUIRE(a1Account.getMasterWeight() == 1);
    REQUIRE(a1Account.getHighThreshold() == 0);
    REQUIRE(a1Account.getLowThreshold() == 0);
    REQUIRE(a1Account.getMidThreshold() == 0);
    REQUIRE(rootAccount.getBalance() ==
            (100000000000000000 - paymentAmount - txfee));

    const uint64_t morePayment = paymentAmount / 2;

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
        LOG(INFO) << "send too little XLM to new account (below reserve)";
        applyPaymentTx(
            app, root, b1, rootSeq++,
            app.getLedgerManager().getCurrentLedgerHeader().baseReserve - 1,
            PAYMENT_LOW_RESERVE);

        AccountFrame bAccount;
        REQUIRE(!AccountFrame::loadAccount(b1.getPublicKey(), bAccount,
                                           app.getDatabase()));
    }

    SECTION("simple credit")
    {
        Currency currency = makeCurrency(root, "IDR");

        SECTION("credit sent to new account (no account error)")
        {
            LOG(INFO) << "credit sent to new account (no account error)";
            applyCreditPaymentTx(app, root, b1, currency, rootSeq++, 100,
                                 PAYMENT_NO_DESTINATION);

            AccountFrame bAccount;
            REQUIRE(!AccountFrame::loadAccount(b1.getPublicKey(), bAccount,
                                               app.getDatabase()));
        }

        SECTION("send XLM with path (not enough offers)")
        {
            LOG(INFO) << "send XLM with path";
            TransactionFramePtr txFrame2 =
                createPaymentTx(root, a1, rootSeq++, morePayment);
            getFirstOperation(*txFrame2).body.paymentOp().path.push_back(
                currency);
            LedgerDelta delta2(
                app.getLedgerManager().getCurrentLedgerHeader());
            txFrame2->apply(delta2, app);

            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*txFrame2)) ==
                    PAYMENT_OVERSENDMAX);
            AccountFrame account;
            REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), account,
                                              app.getDatabase()));
        }

        // actual sendcredit
        SECTION("credit payment with no trust")
        {
            LOG(INFO) << "credit payment with no trust";
            applyCreditPaymentTx(app, root, a1, currency, rootSeq++, 100,
                                 PAYMENT_NO_TRUST);
            AccountFrame account;
            REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), account,
                                              app.getDatabase()));
        }

        SECTION("with trust")
        {
            LOG(INFO) << "with trust";

            applyChangeTrust(app, a1, root, a1Seq++, "IDR", 1000);
            applyCreditPaymentTx(app, root, a1, currency, rootSeq++, 100);

            TrustFrame line;
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), currency, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 100);

            // create b1 account
            applyPaymentTx(app, root, b1, rootSeq++, paymentAmount);

            SequenceNumber b1Seq = getAccountSeqNum(b1, app) + 1;

            applyChangeTrust(app, b1, root, b1Seq++, "IDR", 100);
            applyCreditPaymentTx(app, a1, b1, currency, a1Seq++, 40);

            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), currency, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 60);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), currency, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 40);
            applyCreditPaymentTx(app, b1, root, currency, b1Seq++, 40);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), currency, line,
                                              app.getDatabase()));
            REQUIRE(line.getBalance() == 0);
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
