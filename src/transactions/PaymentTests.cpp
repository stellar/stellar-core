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
#include "lib/json/json.h"
#include "TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerMaster.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentFrame.h"
#include "transactions/ChangeTrustTxFrame.h"

using namespace stellar;
using namespace stellar::txtest;


typedef std::unique_ptr<Application> appPtr;

// *STR Payment 
// *Credit Payment
// STR -> Credit Payment
// Credit -> STR Payment
// Credit -> STR -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx][payment]")
{
    Config const& cfg = getTestConfig();
    Config cfg2(cfg);
    //cfg2.DATABASE = "sqlite3://test.db";
    //cfg2.DATABASE = "postgresql://dbmaster:-island-@localhost/hayashi";
    

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg2);
    Application &app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    uint64_t txfee = app.getLedgerMaster().getTxFee();

    const uint64_t paymentAmount = (uint64_t)app.getLedgerMaster().getMinBalance(0);

    // create an account
    applyPaymentTx(app, root, a1, 1, paymentAmount);
    
    AccountFrame a1Account, rootAccount;
    REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount, app.getDatabase()));
    REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account, app.getDatabase()));
    REQUIRE(rootAccount.getMasterWeight() == 1);
    REQUIRE(rootAccount.getHighThreshold() == 0);
    REQUIRE(rootAccount.getLowThreshold() == 0);
    REQUIRE(rootAccount.getMidThreshold() == 0);
    REQUIRE(a1Account.getBalance() == paymentAmount);
    REQUIRE(a1Account.getMasterWeight() == 1);
    REQUIRE(a1Account.getHighThreshold() == 0);
    REQUIRE(a1Account.getLowThreshold() == 0);
    REQUIRE(a1Account.getMidThreshold() == 0);
    REQUIRE(rootAccount.getBalance() == (100000000000000000 - paymentAmount - txfee));

    const uint64_t morePayment = paymentAmount / 2;

    SECTION("send STR to an existing account")
    {
        applyPaymentTx(app, root, a1, 2, morePayment);
        
        AccountFrame a1Account2, rootAccount2;
        REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount2, app.getDatabase()));
        REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account2, app.getDatabase()));
        REQUIRE(a1Account2.getBalance() == a1Account.getBalance() + morePayment);
        REQUIRE(rootAccount2.getBalance() == (rootAccount.getBalance() - morePayment - txfee));
    }

    SECTION("send to self")
    {
        applyPaymentTx(app, root, root, 2, morePayment);

        AccountFrame rootAccount2;
        REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount2, app.getDatabase()));
        REQUIRE(rootAccount2.getBalance() == (rootAccount.getBalance() - txfee));
    }

    SECTION("send too little STR to new account (below reserve)")
    {
        LOG(INFO) << "send too little STR to new account (below reserve)";
        applyPaymentTx(app,root, b1, 2,
            app.getLedgerMaster().getCurrentLedgerHeader().baseReserve -1,Payment::UNDERFUNDED);

        AccountFrame bAccount;
        REQUIRE(!AccountFrame::loadAccount(b1.getPublicKey(), bAccount, app.getDatabase()));
    }

    SECTION("simple credit")
    {
        Currency currency=makeCurrency(root,"IDR");

        SECTION("credit sent to new account (no account error)")
        {
            LOG(INFO) << "credit sent to new account (no account error)";
            applyCreditPaymentTx(app,root, b1, currency, 2, 100, Payment::NO_DESTINATION);

            AccountFrame bAccount;
            REQUIRE(!AccountFrame::loadAccount(b1.getPublicKey(), bAccount, app.getDatabase()));
        }

        SECTION("send STR with path (not enough offers)")
        {
            LOG(INFO) << "send STR with path";
            TransactionFramePtr txFrame2 = createPaymentTx(root, a1, 2, morePayment);
            getFirstOperation(*txFrame2).body.paymentTx().path.push_back(currency);
            LedgerDelta delta2(app.getLedgerMaster().getCurrentLedgerHeader());
            txFrame2->apply(delta2, app);

            REQUIRE(Payment::getInnerCode(getFirstResult(*txFrame2)) == Payment::OVERSENDMAX);
            AccountFrame account;
            REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), account, app.getDatabase()));
            
        }

        // actual sendcredit
        SECTION("credit payment with no trust")
        {
            LOG(INFO) << "credit payment with no trust";
            applyCreditPaymentTx(app,root, a1, currency, 2, 100, Payment::NO_TRUST);
            AccountFrame account;
            REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), account, app.getDatabase()));
           
        }

        SECTION("with trust")
        {
            LOG(INFO) << "with trust";

            applyChangeTrust(app, a1, root, 1, "IDR", 1000);
            applyCreditPaymentTx(app, root, a1, currency, 2, 100);

            TrustFrame line;
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), currency, line, app.getDatabase()));
            REQUIRE(line.getBalance() == 100);

            // create b1 account
            applyPaymentTx(app,root, b1, 3, paymentAmount);
            applyChangeTrust(app,b1, root, 1, "IDR", 100);
            applyCreditPaymentTx(app,a1, b1, currency, 2, 40);
               
            REQUIRE(TrustFrame::loadTrustLine(a1.getPublicKey(), currency, line, app.getDatabase()));
            REQUIRE(line.getBalance() == 60);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), currency, line, app.getDatabase()));
            REQUIRE(line.getBalance() == 40);
            applyCreditPaymentTx(app,b1, root, currency, 2, 40);
            REQUIRE(TrustFrame::loadTrustLine(b1.getPublicKey(), currency, line, app.getDatabase()));
            REQUIRE(line.getBalance() == 0);
        }
    }
}




